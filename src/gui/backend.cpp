#include "backend.hpp"

#include <QClipboard>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess> // Note: QProcess is not supported on VxWorks, iOS, tvOS, or watchOS.
#include <QUuid>
#include <QDateTime>
#include <QImage>
#include <QPixmap>
#include <QByteArray>
#include <QBuffer>
#include <QPainter>
#include <QImageReader>
#include <QImageWriter>

#include "../neroshop_config.hpp"
#include "../core/version.hpp"
#include "../core/protocol/p2p/serializer.hpp"
#include "daemon_manager.hpp"
//#include "../core/cart.hpp"
#include "../core/protocol/transport/client.hpp"
#include "../core/price/currency_converter.hpp" // neroshop::Converter::is_supported_currency
#include "../core/price/currency_map.hpp"
#include "../core/crypto/sha256.hpp" // sha256
#include "../core/database/database.hpp"
#include "../core/tools/script.hpp"
#include "../core/settings.hpp"
#include "script_controller.hpp" // neroshop::Script::get_table_string
#include "../core/tools/tools.hpp"
#include "../core/tools/logger.hpp"
#include "../core/tools/process.hpp"
#include "../core/category.hpp"
#include "../core/tools/regex.hpp"
#include "../core/crypto/rsa.hpp"
#include "enum_wrapper.hpp"

#include <future>
#include <thread>

namespace neroshop_filesystem = neroshop::filesystem;

neroshop::Backend::Backend(QObject *parent) : QObject(parent) {}

neroshop::Backend::~Backend() {
    #ifdef NEROSHOP_DEBUG
    std::cout << "backend deleted\n";
    #endif
}
//----------------------------------------------------------------
//----------------------------------------------------------------
QString neroshop::Backend::urlToLocalFile(const QUrl &url) const
{
    return url.toLocalFile();
}
//----------------------------------------------------------------
void neroshop::Backend::copyTextToClipboard(const QString& text) {
    QClipboard * clipboard = QGuiApplication::clipboard();
    clipboard->setText(text);
    std::cout << "Copied text to clipboard\n";
}
//----------------------------------------------------------------
//----------------------------------------------------------------
QString neroshop::Backend::imageToBase64(const QImage& image) {
    QByteArray byteArray;
    QBuffer buffer(&byteArray);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG"); // You can choose a different format if needed
    return QString::fromLatin1(byteArray.toBase64().data());
}
//----------------------------------------------------------------
QImage neroshop::Backend::base64ToImage(const QString& base64Data) {
    QByteArray byteArray = QByteArray::fromBase64(base64Data.toLatin1());
    QImageReader reader(byteArray);
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    return image;
}
//----------------------------------------------------------------
//----------------------------------------------------------------
QStringList neroshop::Backend::getCurrencyList() const
{
    QStringList currency_list;
    for (const auto& [key, value] : neroshop::CurrencyMap) {
        currency_list << QString::fromStdString(key);
    }
    return currency_list;
}
//----------------------------------------------------------------
int neroshop::Backend::getCurrencyDecimals(const QString& currency) const {
    auto map_key = currency.toUpper().toStdString();
    // Check if key exists in std::map
    if(neroshop::CurrencyMap.count(map_key) > 0) {
        auto map_value = neroshop::CurrencyMap[map_key];
        int decimal_places = std::get<2>(map_value);
        return decimal_places;
    }
    return 2;
}
//----------------------------------------------------------------
QString neroshop::Backend::getCurrencySign(const QString& currency) const {
    return QString::fromStdString(neroshop::Converter::get_currency_sign(currency.toStdString()));
}
//----------------------------------------------------------------
bool neroshop::Backend::isSupportedCurrency(const QString& currency) const {
    return neroshop::Converter::is_supported_currency(currency.toStdString());
}
//----------------------------------------------------------------
//----------------------------------------------------------------
//----------------------------------------------------------------
void neroshop::Backend::initializeDatabase() {
    db::Sqlite3 * database = neroshop::get_database();
    database->execute("BEGIN;");
    
    // mappings
    if(!database->table_exists("mappings")) { 
        database->execute("CREATE VIRTUAL TABLE mappings USING fts5(search_term, key, content, tokenize='porter unicode61');");
    }
    
    // favorites (wishlists)
    if(!database->table_exists("favorites")) {
        database->execute("CREATE TABLE favorites("
        "user_id TEXT, "
        "listing_key TEXT, "
        "UNIQUE(user_id, listing_key)"
        ");");
    }    
    
    // cart
    if(!database->table_exists("cart")) {
        database->execute("CREATE TABLE cart(uuid TEXT NOT NULL PRIMARY KEY, "
        "user_id TEXT"
        ");");
        // cart_items
        database->execute("CREATE TABLE cart_item(id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
        "cart_id TEXT REFERENCES cart(uuid) ON DELETE CASCADE"
        ");");
        database->execute("ALTER TABLE cart_item ADD COLUMN listing_key TEXT;");
        database->execute("ALTER TABLE cart_item ADD COLUMN quantity INTEGER;");
        database->execute("ALTER TABLE cart_item ADD COLUMN seller_id TEXT;"); // for a multi-vendor cart, specifying the seller_id is important!//database->execute("ALTER TABLE cart_item ADD COLUMN item_weight REAL;");
        database->execute("CREATE UNIQUE INDEX index_cart_item ON cart_item (cart_id, listing_key);"); // cart_id and listing_key duo MUST be unique for each row
    }
    
    // orders (purchase_orders)
    if(!database->table_exists("orders")) { // TODO: rename to order_requests or nah?
        database->execute("CREATE TABLE orders(uuid TEXT NOT NULL PRIMARY KEY);");//database->execute("ALTER TABLE orders ADD COLUMN ?col ?datatype;");
        database->execute("ALTER TABLE orders ADD COLUMN created_at TEXT DEFAULT CURRENT_TIMESTAMP;"); // creation_date // to get UTC time: set to datetime('now');
        //database->execute("ALTER TABLE orders ADD COLUMN number TEXT;"); // uuid
        database->execute("ALTER TABLE orders ADD COLUMN status TEXT;");
        database->execute("ALTER TABLE orders ADD COLUMN customer_id TEXT REFERENCES users(monero_address);"); // the user that placed the order
        // Data below this comment will be stored in order_data as JSON TEXT
        //database->execute("ALTER TABLE orders ADD COLUMN weight REAL;"); // weight of all order items combined - not essential
        database->execute("ALTER TABLE orders ADD COLUMN subtotal INTEGER;");
        database->execute("ALTER TABLE orders ADD COLUMN discount INTEGER;");
        //database->execute("ALTER TABLE orders ADD COLUMN shipping_method TEXT;");
        database->execute("ALTER TABLE orders ADD COLUMN shipping_cost INTEGER;");
        database->execute("ALTER TABLE orders ADD COLUMN total INTEGER;");
        database->execute("ALTER TABLE orders ADD COLUMN payment_option TEXT;"); // escrow (2 of 3), multisig (2 of 2), finalize (no escrow)
        database->execute("ALTER TABLE orders ADD COLUMN coin TEXT;"); // monero, wownero
        database->execute("ALTER TABLE orders ADD COLUMN notes TEXT;"); // additional message for seller
        //database->execute("ALTER TABLE orders ADD COLUMN order_data TEXT;"); // encrypted JSON
        // order_item
        // TODO: remove order_item table and replace it with order_data JSON column
        database->execute("CREATE TABLE order_item(id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
        "order_id TEXT REFERENCES orders(uuid) ON DELETE CASCADE, "
        "product_id TEXT REFERENCES products(uuid), "
        "seller_id TEXT REFERENCES users(monero_address), "
        "quantity INTEGER"
        ");");
        //database->execute("ALTER TABLE order_item ADD COLUMN unit_price ?datatype;");
        //database->execute("ALTER TABLE order_item ADD COLUMN ?col ?datatype;");
    }
    //-------------------------
    database->execute("COMMIT;");
}
//----------------------------------------------------------------
std::string neroshop::Backend::getDatabaseHash() {
    // Get contents from data.sqlite3 file
    std::ifstream rfile (std::string("data.sqlite3").c_str(), std::ios::binary);
    std::stringstream db_content;
    db_content << rfile.rdbuf(); // dump file contents
    rfile.close();
    // Get SHA256sum of data.sqlite3 contents
    std::string sha256sum = neroshop::crypto::sha256(db_content.str());
    std::cout << "sha256sum (data.sqlite3): " << sha256sum << std::endl;
    return sha256sum; // database may have to be closed first in order to get the accurate hash
}
//----------------------------------------------------------------
//----------------------------------------------------------------
QVariantList neroshop::Backend::getCategoryList(bool sort_alphabetically) const {
    QVariantList category_list;
    
    std::vector<Category> categories = predefined_categories; // Make a copy
    
    if (sort_alphabetically) {
        std::sort(categories.begin(), categories.end(), [](const Category& a, const Category& b) {
            return a.name < b.name;
        });
    }
    
    for (const auto& category : categories) {
        QVariantMap category_object;
        category_object.insert("id", category.id);
        category_object.insert("name", QString::fromStdString(category.name));
        category_object.insert("description", QString::fromStdString(category.description));
        category_object.insert("thumbnail", QString::fromStdString(category.thumbnail));
        category_list.append(category_object);
    }

    return category_list;
}

//----------------------------------------------------------------
QVariantList neroshop::Backend::getSubCategoryList(int category_id, bool sort_alphabetically) const {
    QVariantList subcategory_list;
    
    std::vector<Subcategory> subcategories = get_subcategories_by_category_id(category_id);
    
    if (sort_alphabetically) {
        std::sort(subcategories.begin(), subcategories.end(), [](const Subcategory& a, const Subcategory& b) {
            return a.name < b.name;
        });
    }
    
    for (const Subcategory& subcategory : subcategories) {
        QVariantMap subcategory_obj;
        subcategory_obj.insert("id", subcategory.id);
        subcategory_obj.insert("name", QString::fromStdString(subcategory.name));
        subcategory_obj.insert("description", QString::fromStdString(subcategory.description));
        subcategory_obj.insert("thumbnail", QString::fromStdString(subcategory.thumbnail));
        subcategory_obj.insert("category_id", subcategory.category_id);
        
        subcategory_list.append(subcategory_obj);
    }
    
    return subcategory_list;
}
//----------------------------------------------------------------
int neroshop::Backend::getCategoryIdByName(const QString& category_name) const {
    return get_category_id_by_name(category_name.toStdString());
}
//----------------------------------------------------------------
int neroshop::Backend::getSubCategoryIdByName(const QString& subcategory_name) const {
    return get_subcategory_id_by_name(subcategory_name.toStdString());
}
//----------------------------------------------------------------
int neroshop::Backend::getCategoryProductCount(int category_id) const {
    db::Sqlite3 * database = neroshop::get_database();

    std::string query = "SELECT COUNT(*) FROM (SELECT DISTINCT search_term, key FROM mappings WHERE search_term MATCH ?";//"SELECT COUNT(*) FROM (SELECT DISTINCT search_term, key FROM mappings WHERE search_term MATCH ?);";
    std::string category = get_category_name_by_id(category_id);
    
    // Replace ampersands with wildcard (*)
    std::replace(category.begin(), category.end(), '&', '*');
    // Add double quotes around the category for phrase matching
    category = "\"" + category + "\"";

    query += ")";

    int category_product_count = database->get_integer_params(query, { category });
    return category_product_count;
}
//----------------------------------------------------------------
bool neroshop::Backend::hasSubCategory(int category_id) const {
    std::vector<Subcategory> subcategories = get_subcategories_by_category_id(category_id);
    return (!subcategories.empty());
}
//----------------------------------------------------------------
//----------------------------------------------------------------
bool neroshop::Backend::saveAvatarImage(const QString& fileName, const QString& userAccountKey) {
    std::string config_path = NEROSHOP_DEFAULT_CONFIGURATION_PATH;
    std::string cache_folder = config_path + "/" + NEROSHOP_CACHE_FOLDER_NAME;
    std::string avatars_folder = cache_folder + "/" + NEROSHOP_AVATAR_FOLDER_NAME;
    //----------------------------------------
    std::string image_file = fileName.toStdString(); // Full path with file name
    std::string image_name = image_file.substr(image_file.find_last_of("\\/") + 1);// get filename from path (complete base name)
    image_name = image_name.substr(0, image_name.find_last_of(".")); // remove ext
    std::string image_name_hash = neroshop::crypto::sha256(image_name);
    std::string image_ext = image_file.substr(image_file.find_last_of(".") + 1);
    //----------------------------------------
    // datastore/avatars/<account_key>
    std::string key_folder = avatars_folder + "/" + userAccountKey.toStdString();
    if (!neroshop_filesystem::is_directory(key_folder)) {
        if (!neroshop_filesystem::make_directory(key_folder)) {
            neroshop::print("Failed to create folder \"" + key_folder + "\" (ᵕ人ᵕ)!", 1);
            return false;
        }
        neroshop::print("\033[1;97;49mcreated path \"" + key_folder + "\"");
    }
    //----------------------------------------
    // Generate the final destination path
    std::string destinationPath = key_folder + "/" + (image_name_hash + "." + image_ext);
    // Check if image already exists in cache so that we do not export the same image more than once
    if(!neroshop_filesystem::is_file(destinationPath)) {
        // Image Loader crashes when image resolution is too large (ex. 4096 pixels wide) so we need to scale it!!
        QImage sourceImage;
        sourceImage.load(fileName);
        QSize imageSize = sourceImage.size();
        int maxWidth = 200; // Set the maximum width for the resized image
        int maxHeight = 200; // Set the maximum height for the resized image

        // Check if the image size is smaller than the maximum size
        if (imageSize.width() <= maxWidth && imageSize.height() <= maxHeight) {
            // Keep the original image since it's already within the size limits
        } else {
            // Calculate the new size while maintaining the aspect ratio
            QSize newSize = imageSize.scaled(maxWidth, maxHeight, Qt::KeepAspectRatio);

            // Resize the image if it exceeds the maximum dimensions
            if (imageSize != newSize) {
                sourceImage = sourceImage.scaled(newSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
        }

        // Convert the QImage to QPixmap for further processing or saving
        QPixmap resizedPixmap = QPixmap::fromImage(sourceImage);

        // Save the resized image
        resizedPixmap.save(QString::fromStdString(destinationPath));
    }
    neroshop::print("exported \"" + fileName.toStdString() + "\" to \"" + key_folder + "\"", 3);
    return true;    
}
//----------------------------------------------------------------
//----------------------------------------------------------------
bool neroshop::Backend::saveProductThumbnail(const QString& fileName, const QString& listingKey) {
    std::string config_path = NEROSHOP_DEFAULT_CONFIGURATION_PATH;
    std::string cache_folder = config_path + "/" + NEROSHOP_CACHE_FOLDER_NAME;
    std::string listings_folder = cache_folder + "/" + NEROSHOP_CATALOG_FOLDER_NAME;
    //----------------------------------------
    // datastore/listings/<listing_key>
    std::string key_folder = listings_folder + "/" + listingKey.toStdString();
    if (!neroshop_filesystem::is_directory(key_folder)) {
        if (!neroshop_filesystem::make_directory(key_folder)) {
            neroshop::print("Failed to create folder \"" + key_folder + "\" (ᵕ人ᵕ)!", 1);
            return false;
        }
        neroshop::print("\033[1;97;49mcreated path \"" + key_folder + "\"");
    }
    //----------------------------------------
    // Generate the final destination path
    std::string thumbnail_image = "thumbnail.jpg";
    std::string destinationPath = key_folder + "/" + thumbnail_image;
    // Check if image already exists in cache so that we do not export the same image more than once
    if(!neroshop_filesystem::is_file(destinationPath)) {
        // Hopefully the image does not exceed 32 kB in file size :S
        QImage sourceImage;
        sourceImage.load(fileName);
        QSize imageSize = sourceImage.size();
        int maxWidth = 192; // Set the maximum width for the resized image
        int maxHeight = 192; // Set the maximum height for the resized image
        
        // Convert the transparent background to white if necessary
        if (sourceImage.hasAlphaChannel()) {
            QImage convertedImage = QImage(sourceImage.size(), QImage::Format_RGB32);
            convertedImage.fill(Qt::white);
            QPainter painter(&convertedImage);
            painter.drawImage(0, 0, sourceImage);
            painter.end();
            sourceImage = convertedImage;
        }

        // Check if the image size is smaller than the maximum size
        if (imageSize.width() <= maxWidth && imageSize.height() <= maxHeight) {
            // Keep the original image since it's already within the size limits
        } else {
            // Calculate the new size while maintaining the aspect ratio
            QSize newSize = imageSize.scaled(maxWidth, maxHeight, Qt::KeepAspectRatio);

            // Resize the image if it exceeds the maximum dimensions
            if (imageSize != newSize) {
                sourceImage = sourceImage.scaled(newSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
        }

        // Convert the QImage to QPixmap for further processing or saving
        QPixmap resizedPixmap = QPixmap::fromImage(sourceImage);

        // Save the resized image
        resizedPixmap.save(QString::fromStdString(destinationPath), "JPEG");
    }
    
    neroshop::print("exported \"" + thumbnail_image + "\" to \"" + key_folder + "\"", 3);
    return true;
}
//----------------------------------------------------------------
bool neroshop::Backend::saveProductImage(const QString& fileName, const QString& listingKey) {
    std::string config_path = NEROSHOP_DEFAULT_CONFIGURATION_PATH;
    std::string cache_folder = config_path + "/" + NEROSHOP_CACHE_FOLDER_NAME;
    std::string listings_folder = cache_folder + "/" + NEROSHOP_CATALOG_FOLDER_NAME;
    //----------------------------------------
    std::string image_file = fileName.toStdString(); // Full path with file name
    std::string image_name = image_file.substr(image_file.find_last_of("\\/") + 1);// get filename from path (complete base name)
    image_name = image_name.substr(0, image_name.find_last_of(".")); // remove ext
    std::string image_name_hash = neroshop::crypto::sha256(image_name);
    std::string image_ext = image_file.substr(image_file.find_last_of(".") + 1);
    //----------------------------------------    
    // datastore/listings/<listing_key>
    std::string key_folder = listings_folder + "/" + listingKey.toStdString();
    if (!neroshop_filesystem::is_directory(key_folder)) {
        if (!neroshop_filesystem::make_directory(key_folder)) {
            neroshop::print("Failed to create folder \"" + key_folder + "\" (ᵕ人ᵕ)!", 1);
            return false;
        }
        neroshop::print("\033[1;97;49mcreated path \"" + key_folder + "\"");
    }
    //----------------------------------------
    // Generate the final destination path
    std::string destinationPath = key_folder + "/" + (image_name_hash + "." + image_ext);
    // Check if image already exists in cache so that we do not export the same image more than once
    if(!neroshop_filesystem::is_file(destinationPath)) {
        // Image Loader crashes when image resolution is too large (ex. 4096 pixels wide) so we need to scale it!!
        QImage sourceImage;
        sourceImage.load(fileName);
        QSize imageSize = sourceImage.size();
        int maxWidth = 1200; // Set the maximum width for the resized image
        int maxHeight = 1200; // Set the maximum height for the resized image

        // Check if the image size is smaller than the maximum size
        if (imageSize.width() <= maxWidth && imageSize.height() <= maxHeight) {
            // Keep the original image since it's already within the size limits
        } else {
            // Calculate the new size while maintaining the aspect ratio
            QSize newSize = imageSize.scaled(maxWidth, maxHeight, Qt::KeepAspectRatio);

            // Resize the image if it exceeds the maximum dimensions
            if (imageSize != newSize) {
                sourceImage = sourceImage.scaled(newSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
        }

        // Convert the QImage to QPixmap for further processing or saving
        QPixmap resizedPixmap = QPixmap::fromImage(sourceImage);

        // Save the resized image
        resizedPixmap.save(QString::fromStdString(destinationPath));
    }
    neroshop::print("exported \"" + fileName.toStdString() + "\" to \"" + key_folder + "\"", 3);
    return true;
}
//----------------------------------------------------------------
QVariantMap neroshop::Backend::uploadProductImage(const QString& fileName, int image_id) {
    QVariantMap image;
    //----------------------------------------
    // Read image from file and retrieve its contents
    std::ifstream product_image_file(fileName.toStdString(), std::ios::binary); // std::ios::binary is the same as std::ifstream::binary
    if(!product_image_file.good()) {
        std::cout << NEROSHOP_TAG "failed to load " << fileName.toStdString() << std::endl; 
        return {};
    }
    product_image_file.seekg(0, std::ios::end);
    size_t size = static_cast<int>(product_image_file.tellg()); // in bytes
    // Limit product image size to 12582912 bytes (12 megabyte)
    const int max_bytes = 12582912;
    double kilobytes = max_bytes / 1024.0;
    double megabytes = kilobytes / 1024.0;
    if(size >= max_bytes) {
        neroshop::print("Product upload image cannot exceed " + std::to_string(megabytes) + " MB (twelve megabyte)", 1);
        return {};
    }
    product_image_file.seekg(0);
    std::vector<unsigned char> buffer(size);
    if(!product_image_file.read(reinterpret_cast<char *>(&buffer[0]), size)) {
        std::cout << NEROSHOP_TAG "error: only " << product_image_file.gcount() << " could be read";
        return {}; // exit function
    }
    product_image_file.close();
    //----------------------------------------
    // Create the image VariantMap (object)
    std::string image_file = fileName.toStdString(); // Full path with file name
    std::string image_name = image_file.substr(image_file.find_last_of("\\/") + 1);
    image_name = image_name.substr(0, image_name.find_last_of(".")); // remove ext
    std::string image_name_hash = neroshop::crypto::sha256(image_name);
    std::string image_ext = image_file.substr(image_file.find_last_of(".") + 1);
    image["name"] = QString::fromStdString(image_name_hash + "." + image_ext);//fileName;
    qint64 imageSize64 = static_cast<qint64>(size);
    image["size"] = QVariant::fromValue(imageSize64);
    image["id"] = image_id;
    image["source"] = fileName;
    //----------------------------------------
    return image;
}
//----------------------------------------------------------------
//----------------------------------------------------------------
int neroshop::Backend::getProductStarCount(const QVariantList& product_ratings) {
    // Get total number of star ratings for a specific product
    return product_ratings.size();
}
//----------------------------------------------------------------
int neroshop::Backend::getProductStarCount(const QString& product_id) {    
    QVariantList product_ratings = getProductRatings(product_id);
    return getProductStarCount(product_ratings);
}
//----------------------------------------------------------------
int neroshop::Backend::getProductStarCount(const QVariantList& product_ratings, int star_number) {
    // Get total number of N star ratings for a specific product
    if(star_number > 5) star_number = 5;
    if(star_number < 1) star_number = 1;
    int star_count = 0;
    for (const QVariant& variant : product_ratings) {
        QVariantMap rating_obj = variant.toMap();
        int rating_stars = rating_obj["stars"].toInt();
        if (rating_stars == star_number) {
            star_count++;
        }
    }
    return star_count;
}
//----------------------------------------------------------------
int neroshop::Backend::getProductStarCount(const QString& product_id, int star_number) {
    QVariantList product_ratings = getProductRatings(product_id);
    return getProductStarCount(product_id, star_number);
}
//----------------------------------------------------------------
float neroshop::Backend::getProductAverageStars(const QVariantList& product_ratings) {
    // Get number of star ratings for a specific product
    int total_star_ratings = product_ratings.size();
    if(total_star_ratings == 0) return 0.0f;
    // Get number of 1, 2, 3, 4, and 5 star_ratings
    int one_star_count = 0, two_star_count = 0, three_star_count = 0, four_star_count = 0, five_star_count = 0;
    for (const QVariant& variant : product_ratings) {
        QVariantMap rating_obj = variant.toMap();
        int rating_stars = rating_obj["stars"].toInt();
        if (rating_stars == 1) {
            one_star_count++;
        } else if (rating_stars == 2) {
            two_star_count++;
        } else if (rating_stars == 3) {
            three_star_count++;
        } else if (rating_stars == 4) {
            four_star_count++;
        } else if (rating_stars == 5) {
            five_star_count++;
        }
    }
    // Now calculate the average stars 
    float average_stars = (
        (1 * static_cast<float>(one_star_count)) + 
        (2 * static_cast<float>(two_star_count)) + 
        (3 * static_cast<float>(three_star_count)) + 
        (4 * static_cast<float>(four_star_count)) + 
        (5 * static_cast<float>(five_star_count))) / total_star_ratings;
    return average_stars;
}
//----------------------------------------------------------------
float neroshop::Backend::getProductAverageStars(const QString& product_id) {
    QVariantList product_ratings = getProductRatings(product_id);
    return getProductAverageStars(product_ratings);
}
//----------------------------------------------------------------
QVariantList neroshop::Backend::getProductRatings(const QString& product_id) {
    Client * client = Client::get_main_client();
    //----------------------------------
    std::string command = "SELECT DISTINCT key FROM mappings WHERE search_term = $1 AND content = 'product_rating'";
    db::Sqlite3 * database = neroshop::get_database();
    sqlite3_stmt * stmt = nullptr;
    // Prepare (compile) statement
    if(sqlite3_prepare_v2(database->get_handle(), command.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        neroshop::print("sqlite3_prepare_v2: " + std::string(sqlite3_errmsg(database->get_handle())), 1);
        return {};
    }
    // Bind value to parameter arguments
    QByteArray productIdByteArray = product_id.toUtf8();
    if(sqlite3_bind_text(stmt, 1, productIdByteArray.constData(), productIdByteArray.length(), SQLITE_STATIC) != SQLITE_OK) {
        neroshop::print("sqlite3_bind_text: " + std::string(sqlite3_errmsg(database->get_handle())), 1);
        sqlite3_finalize(stmt);
        return {};
    }
    //----------------------------------
    QVariantList product_ratings;
    // Get all table values row by row
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        for(int i = 0; i < sqlite3_column_count(stmt); i++) { 
            QVariantMap product_rating_obj; // Create object for each key (row)
            
            std::string column_value = (sqlite3_column_text(stmt, i) == nullptr) ? "NULL" : reinterpret_cast<const char *>(sqlite3_column_text(stmt, i));//std::cout << column_value  << " (" << i << ")" << std::endl;
            if(column_value == "NULL") continue; // Skip invalid columns
            QString key = QString::fromStdString(column_value);//std::cout << key.toStdString() << "\n";
            
            // Get the value of the corresponding key from the DHT
            std::string response;
            client->get(key.toStdString(), response); // TODO: error handling
            std::cout << "Received response (get): " << response << "\n";
            // Parse the response
            nlohmann::json json = nlohmann::json::parse(response);
            if(json.contains("error")) {
                int rescode = database->execute_params("DELETE FROM mappings WHERE key = ?1", { key.toStdString() });
                if(rescode != SQLITE_OK) neroshop::print("sqlite error: DELETE failed", 1);
                //emit productRatingsChanged();
                continue; // Key is lost or missing from DHT, skip to next iteration
            }
            
            const auto& response_obj = json["response"];
            assert(response_obj.is_object());
            if (response_obj.contains("value") && response_obj["value"].is_string()) {
                const auto& value = response_obj["value"].get<std::string>();
                nlohmann::json value_obj = nlohmann::json::parse(value);
                assert(value_obj.is_object());//std::cout << value_obj.dump(4) << "\n";
                std::string metadata = value_obj["metadata"].get<std::string>();
                if (metadata != "product_rating") { std::cerr << "Invalid metadata. \"product_rating\" expected, got \"" << metadata << "\" instead\n"; continue; }
                product_rating_obj.insert("key", key);
                product_rating_obj.insert("rater_id", QString::fromStdString(value_obj["rater_id"].get<std::string>()));
                product_rating_obj.insert("comments", QString::fromStdString(value_obj["comments"].get<std::string>()));
                product_rating_obj.insert("signature", QString::fromStdString(value_obj["signature"].get<std::string>()));
                product_rating_obj.insert("stars", value_obj["stars"].get<int>());
            }
            
            product_ratings.append(product_rating_obj);
        }
    }
    //----------------------------------
    return product_ratings;
}
//----------------------------------------------------------------
//----------------------------------------------------------------
int neroshop::Backend::getSellerGoodRatings(const QVariantList& seller_ratings) {
    int good_ratings_count = 0;
    // Get seller's good (positive) ratings
    for (const QVariant& variant : seller_ratings) {
        QVariantMap rating_obj = variant.toMap();
        int rating_score = rating_obj["score"].toInt();
        if(rating_score == 1) {
            good_ratings_count++;
        }
    }
    return good_ratings_count;
}
//----------------------------------------------------------------
int neroshop::Backend::getSellerGoodRatings(const QString& user_id) {
    QVariantList seller_ratings = getSellerRatings(user_id);
    return getSellerGoodRatings(seller_ratings);
}
//----------------------------------------------------------------
int neroshop::Backend::getSellerBadRatings(const QVariantList& seller_ratings) {
    int bad_ratings_count = 0;
    // Get seller's bad (negative) ratings
    for (const QVariant& variant : seller_ratings) {
        QVariantMap rating_obj = variant.toMap();
        int rating_score = rating_obj["score"].toInt();
        if(rating_score == 0) {
            bad_ratings_count++;
        }
    }    
    return bad_ratings_count;
}
//----------------------------------------------------------------
int neroshop::Backend::getSellerBadRatings(const QString& user_id) {
    QVariantList seller_ratings = getSellerRatings(user_id);
    return getSellerBadRatings(seller_ratings);
}
//----------------------------------------------------------------
int neroshop::Backend::getSellerRatingsCount(const QVariantList& seller_ratings) {
    return seller_ratings.size();
}
//----------------------------------------------------------------
int neroshop::Backend::getSellerRatingsCount(const QString& user_id) {
    QVariantList seller_ratings = getSellerRatings(user_id);
    return getSellerRatingsCount(seller_ratings);
}
//----------------------------------------------------------------
int neroshop::Backend::getSellerReputation(const QVariantList& seller_ratings) {
    int good_ratings_count = 0, bad_ratings_count = 0;
    int ratings_count = seller_ratings.size();
    if(ratings_count <= 0) return 0; // seller has not yet been rated so his or her reputation will be 0%
    // Get seller's good (positive) ratings
    for (const QVariant& variant : seller_ratings) {
        QVariantMap rating_obj = variant.toMap();
        int rating_score = rating_obj["score"].toInt();
        if(rating_score == 1) {
            good_ratings_count++;
        }
    }
    // Calculate seller reputation
    double reputation = (good_ratings_count / static_cast<double>(ratings_count)) * 100;
    return static_cast<int>(reputation); // convert reputation to an integer (for easier readability)
}
//----------------------------------------------------------------
int neroshop::Backend::getSellerReputation(const QString& user_id) {
    QVariantList seller_ratings = getSellerRatings(user_id);
    return getSellerReputation(seller_ratings);
}
//----------------------------------------------------------------
// returns an array of ratings objects
QVariantList neroshop::Backend::getSellerRatings(const QString& user_id) {
    Client * client = Client::get_main_client();
    //----------------------------------
    std::string command = "SELECT DISTINCT key FROM mappings WHERE search_term = $1 AND content = 'seller_rating'";
    db::Sqlite3 * database = neroshop::get_database();
    sqlite3_stmt * stmt = nullptr;
    // Prepare (compile) statement
    if(sqlite3_prepare_v2(database->get_handle(), command.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        neroshop::print("sqlite3_prepare_v2: " + std::string(sqlite3_errmsg(database->get_handle())), 1);
        return {};
    }
    // Bind value to parameter arguments
    QByteArray userIdByteArray = user_id.toUtf8();
    if(sqlite3_bind_text(stmt, 1, userIdByteArray.constData(), userIdByteArray.length(), SQLITE_STATIC) != SQLITE_OK) {
        neroshop::print("sqlite3_bind_text: " + std::string(sqlite3_errmsg(database->get_handle())), 1);
        sqlite3_finalize(stmt);
        return {};
    }
    //----------------------------------
    QVariantList seller_ratings;
    // Get all table values row by row
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        for(int i = 0; i < sqlite3_column_count(stmt); i++) { 
            QVariantMap seller_rating_obj; // Create object for each key (row)
            
            std::string column_value = (sqlite3_column_text(stmt, i) == nullptr) ? "NULL" : reinterpret_cast<const char *>(sqlite3_column_text(stmt, i));//std::cout << column_value  << " (" << i << ")" << std::endl;
            if(column_value == "NULL") continue; // Skip invalid columns
            QString key = QString::fromStdString(column_value);//std::cout << key.toStdString() << "\n";
            
            // Get the value of the corresponding key from the DHT
            std::string response;
            client->get(key.toStdString(), response); // TODO: error handling
            std::cout << "Received response (get): " << response << "\n";
            // Parse the response
            nlohmann::json json = nlohmann::json::parse(response);
            if(json.contains("error")) {
                int rescode = database->execute_params("DELETE FROM mappings WHERE key = ?1", { key.toStdString() });
                if(rescode != SQLITE_OK) neroshop::print("sqlite error: DELETE failed", 1);
                //emit sellerRatingsChanged();
                continue; // Key is lost or missing from DHT, skip to next iteration
            }
            
            const auto& response_obj = json["response"];
            assert(response_obj.is_object());
            if (response_obj.contains("value") && response_obj["value"].is_string()) {
                const auto& value = response_obj["value"].get<std::string>();
                nlohmann::json value_obj = nlohmann::json::parse(value);
                assert(value_obj.is_object());//std::cout << value_obj.dump(4) << "\n";
                std::string metadata = value_obj["metadata"].get<std::string>();
                if (metadata != "seller_rating") { std::cerr << "Invalid metadata. \"seller_rating\" expected, got \"" << metadata << "\" instead\n"; continue; }
                seller_rating_obj.insert("key", key);
                seller_rating_obj.insert("rater_id", QString::fromStdString(value_obj["rater_id"].get<std::string>()));
                seller_rating_obj.insert("comments", QString::fromStdString(value_obj["comments"].get<std::string>()));
                seller_rating_obj.insert("signature", QString::fromStdString(value_obj["signature"].get<std::string>()));
                seller_rating_obj.insert("score", value_obj["score"].get<int>());
            }
            
            seller_ratings.append(seller_rating_obj);
        }
    }
    //----------------------------------
    return seller_ratings;
}
//----------------------------------------------------------------
//----------------------------------------------------------------
QString neroshop::Backend::getDisplayNameByUserId(const QString& user_id) {
    db::Sqlite3 * database = neroshop::get_database();
    if(!database) throw std::runtime_error("database is NULL");
    std::string key = database->get_text_params("SELECT key FROM mappings WHERE search_term = ?1 AND content = 'account' LIMIT 1;", { user_id.toStdString() });
    if(key.empty()) return user_id; // Key will never be empty as long as it exists in DHT + database

    std::string display_name = database->get_text_params("SELECT search_term FROM mappings WHERE key = ?1 AND LENGTH(search_term) <= 30 AND content = 'account'", { key });
    if(!display_name.empty()) {
        return QString::fromStdString(display_name);
    }
    // If the display name happens to be empty then it means the user's account (DHT) key is lost or missing
    // or more often than not it is because the user did not set a display name, so deleting the key from the database for this reason is stupid, dangerous, and will have unintended consequences
    // so its best to just return the user_id
    if(display_name.empty()) {
        // do nothing
    }
    return user_id;
}

QString neroshop::Backend::getKeyByUserId(const QString& user_id) { // not currently in use
    db::Sqlite3 * database = neroshop::get_database();
    if(!database) throw std::runtime_error("database is NULL");
    std::string key = database->get_text_params("SELECT key FROM mappings WHERE search_term = $1 AND content = 'account' LIMIT 1;", { user_id.toStdString() });
    return QString::fromStdString(key);
}
//----------------------------------------------------------------
QVariantMap neroshop::Backend::getUser(const QString& user_id) {
    Client * client = Client::get_main_client();
    
    db::Sqlite3 * database = neroshop::get_database();
    if(!database) throw std::runtime_error("database is NULL");
    std::string key = database->get_text_params("SELECT key FROM mappings WHERE search_term = $1 AND content = 'account' LIMIT 1;", { user_id.toStdString() });
    if(key.empty()) return {};
    // Get the value of the corresponding key from the DHT
    std::string response;
    client->get(key, response); // TODO: error handling
    std::cout << "Received response (get): " << response << "\n";
    // Parse the response
    nlohmann::json json = nlohmann::json::parse(response);
    if(json.contains("error")) {
        int rescode = database->execute_params("DELETE FROM mappings WHERE key = ?1", { key });
        if(rescode != SQLITE_OK) neroshop::print("sqlite error: DELETE failed", 1);
        return {}; // Key is lost or missing from DHT, skip to next iteration
    }
    
    QVariantMap user_object;
            
    const auto& response_obj = json["response"];
    assert(response_obj.is_object());
    if (response_obj.contains("value") && response_obj["value"].is_string()) {
        const auto& value = response_obj["value"].get<std::string>();
        nlohmann::json value_obj = nlohmann::json::parse(value);
        assert(value_obj.is_object());//std::cout << value_obj.dump(4) << "\n";
        std::string metadata = value_obj["metadata"].get<std::string>();
        if (metadata != "user") { std::cerr << "Invalid metadata. \"user\" expected, got \"" << metadata << "\" instead\n"; return {}; }
        user_object.insert("key", QString::fromStdString(key));
        if(value_obj.contains("display_name") && value_obj["display_name"].is_string()) {
            std::string display_name = value_obj["display_name"].get<std::string>();
            user_object.insert("display_name", QString::fromStdString(display_name));
        }
        user_object.insert("monero_address", QString::fromStdString(value_obj["monero_address"].get<std::string>()));
        user_object.insert("user_id", QString::fromStdString(value_obj["monero_address"].get<std::string>())); // alias
        user_object.insert("public_key", QString::fromStdString(value_obj["public_key"].get<std::string>()));
        if(value_obj.contains("avatar") && value_obj["avatar"].is_object()) {
            const auto& avatar_obj = value_obj["avatar"];
            QVariantMap avatar;
            avatar.insert("name", QString::fromStdString(avatar_obj["name"].get<std::string>()));
            user_object.insert("avatar", avatar);
        }
        user_object.insert("signature", QString::fromStdString(value_obj["signature"].get<std::string>()));
    }

    return user_object;
}
//----------------------------------------------------------------
//----------------------------------------------------------------
int neroshop::Backend::getCartMaximumItems() {
    return neroshop::Cart::get_max_items();
}
//----------------------------------------------------------------
int neroshop::Backend::getCartMaximumQuantity() {
    return neroshop::Cart::get_max_quantity();
}
//----------------------------------------------------------------
// not really used at the moment
int neroshop::Backend::getStockAvailable(const QString& product_id) {
    Client * client = Client::get_main_client();

    db::Sqlite3 * database = neroshop::get_database();
    if(!database) throw std::runtime_error("database is NULL");
    std::string key = database->get_text_params("SELECT key FROM mappings WHERE search_term = $1 AND content = 'listing'", { product_id.toStdString() });
    if(key.empty()) return 0;
    // Get the value of the corresponding key from the DHT
    std::string response;
    client->get(key, response); // TODO: error handling
    std::cout << "Received response (get): " << response << "\n";
    // Parse the response
    nlohmann::json json = nlohmann::json::parse(response);
    if(json.contains("error")) {
        int rescode = database->execute_params("DELETE FROM mappings WHERE key = ?1", { key });
        if(rescode != SQLITE_OK) neroshop::print("sqlite error: DELETE failed", 1);
        return 0; // Key is lost or missing from DHT, return 
    }    
    
    const auto& response_obj = json["response"];
    assert(response_obj.is_object());
    if (response_obj.contains("value") && response_obj["value"].is_string()) {
        const auto& value = response_obj["value"].get<std::string>();
        nlohmann::json value_obj = nlohmann::json::parse(value);
        assert(value_obj.is_object());//std::cout << value_obj.dump(4) << "\n";
        std::string metadata = value_obj["metadata"].get<std::string>();
        if (metadata != "listing") { std::cerr << "Invalid metadata. \"listing\" expected, got \"" << metadata << "\" instead\n"; return 0; }
        int quantity = value_obj["quantity"].get<int>();
        return quantity;
    }
    
    return 0;
}
//----------------------------------------------------------------
//----------------------------------------------------------------
QVariantList neroshop::Backend::getInventory(const QString& user_id, bool hide_illicit_items) {
    Client * client = Client::get_main_client();
    
    neroshop::db::Sqlite3 * database = neroshop::get_database();
    if(!database) throw std::runtime_error("database is NULL");
    std::string command = "SELECT DISTINCT key FROM mappings WHERE search_term = ?1 AND content = 'listing'";
    sqlite3_stmt * stmt = nullptr;
    // Prepare (compile) statement
    if(sqlite3_prepare_v2(database->get_handle(), command.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        neroshop::print("sqlite3_prepare_v2: " + std::string(sqlite3_errmsg(database->get_handle())), 1);
        return {};
    }
    // Bind user_id to TEXT
    QByteArray userIdByteArray = user_id.toUtf8();
    if(sqlite3_bind_text(stmt, 1, userIdByteArray.constData(), userIdByteArray.length(), SQLITE_STATIC) != SQLITE_OK) {
        neroshop::print("sqlite3_bind_text (arg: 1): " + std::string(sqlite3_errmsg(database->get_handle())), 1);
        sqlite3_finalize(stmt);
        return {};
    }    
    // Check whether the prepared statement returns no data (for example an UPDATE)
    if(sqlite3_column_count(stmt) == 0) {
        neroshop::print("No data found. Be sure to use an appropriate SELECT statement", 1);
        return {};
    }
    
    QVariantList inventory_array;
    // Get all table values row by row
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        QVariantMap inventory_object; // Create an object for each row
        QVariantList product_images;
        QStringList product_categories;

        for(int i = 0; i < sqlite3_column_count(stmt); i++) {
            std::string column_value = (sqlite3_column_text(stmt, i) == nullptr) ? "NULL" : reinterpret_cast<const char *>(sqlite3_column_text(stmt, i));//std::cout << column_value  << " (" << i << ")" << std::endl;
            if(column_value == "NULL") continue; // Skip invalid columns
            QString key = QString::fromStdString(column_value);
            // Get the value of the corresponding key from the DHT
            std::string response;
            client->get(key.toStdString(), response); // TODO: error handling
            std::cout << "Received response (get): " << response << "\n";
            // Parse the response
            nlohmann::json json = nlohmann::json::parse(response);
            if(json.contains("error")) {
                int rescode = database->execute_params("DELETE FROM mappings WHERE key = ?1", { key.toStdString() });
                if(rescode != SQLITE_OK) neroshop::print("sqlite error: DELETE failed", 1);
                continue; // Key is lost or missing from DHT, skip to next iteration
            }
            
            const auto& response_obj = json["response"];
            assert(response_obj.is_object());
            if (response_obj.contains("value") && response_obj["value"].is_string()) {
                const auto& value = response_obj["value"].get<std::string>();
                nlohmann::json value_obj = nlohmann::json::parse(value);
                assert(value_obj.is_object());//std::cout << value_obj.dump(4) << "\n";
                std::string metadata = value_obj["metadata"].get<std::string>();
                if (metadata != "listing") { std::cerr << "Invalid metadata. \"listing\" expected, got \"" << metadata << "\" instead\n"; continue; }
                inventory_object.insert("key", key);
                inventory_object.insert("listing_uuid", QString::fromStdString(value_obj["id"].get<std::string>()));
                inventory_object.insert("seller_id", QString::fromStdString(value_obj["seller_id"].get<std::string>()));
                inventory_object.insert("quantity", value_obj["quantity"].get<int>());
                inventory_object.insert("price", value_obj["price"].get<double>());
                inventory_object.insert("currency", QString::fromStdString(value_obj["currency"].get<std::string>()));
                inventory_object.insert("condition", QString::fromStdString(value_obj["condition"].get<std::string>()));
                if(value_obj.contains("location") && value_obj["location"].is_string()) {
                    inventory_object.insert("location", QString::fromStdString(value_obj["location"].get<std::string>()));
                }
                inventory_object.insert("date", QString::fromStdString(value_obj["date"].get<std::string>()));
                assert(value_obj["product"].is_object());
                const auto& product_obj = value_obj["product"];
                inventory_object.insert("product_uuid", QString::fromStdString(product_obj["id"].get<std::string>()));
                inventory_object.insert("product_name", QString::fromStdString(product_obj["name"].get<std::string>()));
                inventory_object.insert("product_description", QString::fromStdString(product_obj["description"].get<std::string>()));
                // product category and subcategories
                std::string category = product_obj["category"].get<std::string>();
                product_categories.append(QString::fromStdString(category));
                if (product_obj.contains("subcategories") && product_obj["subcategories"].is_array()) {
                    const auto& subcategories_array = product_obj["subcategories"];
                    for (const auto& subcategory : subcategories_array) {
                        if (subcategory.is_string()) {
                            product_categories.append(QString::fromStdString(subcategory.get<std::string>()));
                        }
                    }
                    inventory_object.insert("product_categories", product_categories);
                }
                //inventory_object.insert("", QString::fromStdString(product_obj[""].get<std::string>()));
                if (product_obj.contains("images") && product_obj["images"].is_array()) {
                    const auto& images_array = product_obj["images"];
                    for (const auto& image : images_array) {
                        if (image.contains("name") && image.contains("id")) {
                            const auto& image_name = image["name"].get<std::string>();
                            const auto& image_id = image["id"].get<int>();
                            
                            QVariantMap image_map;
                            image_map.insert("name", QString::fromStdString(image_name));
                            image_map.insert("id", image_id);
                            product_images.append(image_map);
                        }
                    }
                    inventory_object.insert("product_images", product_images);
                }
                if (product_obj.contains("thumbnail") && product_obj["thumbnail"].is_string()) {
                    inventory_object.insert("product_thumbnail", QString::fromStdString(product_obj["thumbnail"].get<std::string>()));
                }
                // Skip products with illegal categories/subcategories
                if (hide_illicit_items) {
                    if(isIllicitItem(inventory_object)) {
                        continue;
                    }
                }
            }
            inventory_array.append(inventory_object);
        }
    }
        
    sqlite3_finalize(stmt);

    return inventory_array;
}
//----------------------------------------------------------------
//----------------------------------------------------------------
QVariantList neroshop::Backend::getListingsBySearchTerm(const QString& searchTerm, int count, bool hide_illicit_items) {
    // Transition from Sqlite to DHT:
    Client * client = Client::get_main_client();
    db::Sqlite3 * database = neroshop::get_database();
    if(!database) throw std::runtime_error("database is NULL");
    
    std::string command = "SELECT DISTINCT key FROM mappings WHERE (search_term MATCH ?1 OR search_term MATCH ?1 || '*') AND (content = 'listing') LIMIT ?2;";//"SELECT DISTINCT key FROM mappings WHERE (search_term MATCH ? OR search_term LIKE '%' || ? || '%' COLLATE NOCASE) AND (content MATCH 'listing');";//"SELECT DISTINCT key FROM mappings WHERE search_term MATCH ? AND content MATCH 'listing';";
    sqlite3_stmt * stmt = nullptr;
    // Prepare (compile) statement
    if(sqlite3_prepare_v2(database->get_handle(), command.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        neroshop::print("sqlite3_prepare_v2: " + std::string(sqlite3_errmsg(database->get_handle())), 1);
        return {};
    }
    //-------------------------------------------------------
    // Bind value to parameter arguments
    QByteArray searchTermByteArray = searchTerm.toUtf8(); // Convert QString to std::string equivalent
    if(sqlite3_bind_text(stmt, 1, searchTermByteArray.constData(), searchTermByteArray.length(), SQLITE_STATIC) != SQLITE_OK) {
        neroshop::print("sqlite3_bind_text: " + std::string(sqlite3_errmsg(database->get_handle())), 1);
        sqlite3_finalize(stmt);
        return {};//database->execute("ROLLBACK;"); return {};
    }        
    
    if(sqlite3_bind_int(stmt, 2, count) != SQLITE_OK) {
        neroshop::print("sqlite3_bind_int: " + std::string(sqlite3_errmsg(database->get_handle())), 1);
        sqlite3_finalize(stmt);
        return {};//database->execute("ROLLBACK;"); return {};
    }            
    //-------------------------------------------------------
    // Check whether the prepared statement returns no data (for example an UPDATE)
    if(sqlite3_column_count(stmt) == 0) {
        neroshop::print("No data found. Be sure to use an appropriate SELECT statement", 1);
        return {};
    }
    
    QVariantList catalog;
    // Get all table values row by row
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        for(int i = 0; i < sqlite3_column_count(stmt); i++) {
            QVariantMap listing; // Create an object for each row
            QVariantList product_images;
            QStringList product_categories;
            
            std::string column_value = (sqlite3_column_text(stmt, i) == nullptr) ? "NULL" : reinterpret_cast<const char *>(sqlite3_column_text(stmt, i));//std::cout << column_value  << " (" << i << ")" << std::endl;
            if(column_value == "NULL") continue; // Skip invalid columns
            QString key = QString::fromStdString(column_value);
            // Get the value of the corresponding key from the DHT
            std::string response;
            client->get(key.toStdString(), response); // TODO: error handling
            std::cout << "Received response (get): " << response << "\n";
            // Parse the response
            nlohmann::json json = nlohmann::json::parse(response);
            if(json.contains("error")) { 
                int rescode = database->execute_params("DELETE FROM mappings WHERE key = ?1", { key.toStdString() });
                if(rescode != SQLITE_OK) neroshop::print("sqlite error: DELETE failed", 1);
                //emit categoryProductCountChanged();//(category_id);
                //emit searchResultsChanged();
                continue; // Key is lost or missing from DHT, skip to next iteration
            }
            
            const auto& response_obj = json["response"];
            assert(response_obj.is_object());
            if (response_obj.contains("value") && response_obj["value"].is_string()) {
                const auto& value = response_obj["value"].get<std::string>();
                nlohmann::json value_obj = nlohmann::json::parse(value);
                assert(value_obj.is_object());//std::cout << value_obj.dump(4) << "\n";
                std::string metadata = value_obj["metadata"].get<std::string>();
                listing.insert("key", key);
                listing.insert("listing_uuid", QString::fromStdString(value_obj["id"].get<std::string>()));
                listing.insert("seller_id", QString::fromStdString(value_obj["seller_id"].get<std::string>()));
                listing.insert("quantity", value_obj["quantity"].get<int>());
                listing.insert("price", value_obj["price"].get<double>());
                listing.insert("currency", QString::fromStdString(value_obj["currency"].get<std::string>()));
                listing.insert("condition", QString::fromStdString(value_obj["condition"].get<std::string>()));
                if(value_obj.contains("location") && value_obj["location"].is_string()) {
                    listing.insert("location", QString::fromStdString(value_obj["location"].get<std::string>()));
                }
                listing.insert("date", QString::fromStdString(value_obj["date"].get<std::string>()));
                assert(value_obj["product"].is_object());
                const auto& product_obj = value_obj["product"];
                listing.insert("product_uuid", QString::fromStdString(product_obj["id"].get<std::string>()));
                listing.insert("product_name", QString::fromStdString(product_obj["name"].get<std::string>()));
                listing.insert("product_description", QString::fromStdString(product_obj["description"].get<std::string>()));
                // product category and subcategories
                std::string category = product_obj["category"].get<std::string>();
                product_categories.append(QString::fromStdString(category));
                if (product_obj.contains("subcategories") && product_obj["subcategories"].is_array()) {
                    const auto& subcategories_array = product_obj["subcategories"];
                    for (const auto& subcategory : subcategories_array) {
                        if (subcategory.is_string()) {
                            product_categories.append(QString::fromStdString(subcategory.get<std::string>()));
                        }
                    }
                    listing.insert("product_categories", product_categories);
                }
                //listing.insert("", QString::fromStdString(product_obj[""].get<std::string>()));
                //listing.insert("", QString::fromStdString(product_obj[""].get<std::string>()));
                if (product_obj.contains("images") && product_obj["images"].is_array()) {
                    const auto& images_array = product_obj["images"];
                    for (const auto& image : images_array) {
                        if (image.contains("name") && image.contains("id")) {
                            const auto& image_name = image["name"].get<std::string>();
                            const auto& image_id = image["id"].get<int>();
                            
                            QVariantMap image_map;
                            image_map.insert("name", QString::fromStdString(image_name));
                            image_map.insert("id", image_id);
                            product_images.append(image_map);
                        }
                    }
                    listing.insert("product_images", product_images);
                }
                // Skip products with illegal categories/subcategories
                if (hide_illicit_items) {
                    if(isIllicitItem(listing)) {
                        continue;
                    }
                }
            }
            // Append to catalog only if the key was found successfully
            catalog.append(listing);
        }
    }
    
    sqlite3_finalize(stmt);

    return catalog;
}
//----------------------------------------------------------------
QVariantList neroshop::Backend::getListings(ListingSorting sorting, bool hide_illicit_items) {
    // Transition from Sqlite to DHT:
    Client * client = Client::get_main_client();
    db::Sqlite3 * database = neroshop::get_database();
    if(!database) throw std::runtime_error("database is NULL");
    
    std::string command = "SELECT DISTINCT key FROM mappings WHERE content MATCH 'listing';";
    sqlite3_stmt * stmt = nullptr;
    // Prepare (compile) statement
    if(sqlite3_prepare_v2(database->get_handle(), command.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        neroshop::print("sqlite3_prepare_v2: " + std::string(sqlite3_errmsg(database->get_handle())), 1);
        return {};
    }
    // Check whether the prepared statement returns no data (for example an UPDATE)
    if(sqlite3_column_count(stmt) == 0) {
        neroshop::print("No data found. Be sure to use an appropriate SELECT statement", 1);
        return {};
    }
    
    QVariantList catalog;
    // Get all table values row by row
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        QVariantMap listing; // Create an object for each row
        QVariantList product_images;
        QStringList product_categories;

        for(int i = 0; i < sqlite3_column_count(stmt); i++) {
            std::string column_value = (sqlite3_column_text(stmt, i) == nullptr) ? "NULL" : reinterpret_cast<const char *>(sqlite3_column_text(stmt, i));//std::cout << column_value  << " (" << i << ")" << std::endl;
            if(column_value == "NULL") continue; // Skip invalid columns
            QString key = QString::fromStdString(column_value);
            // Get the value of the corresponding key from the DHT
            std::string response;
            client->get(key.toStdString(), response); // TODO: error handling
            std::cout << "Received response (get): " << response << "\n";
            // Parse the response
            nlohmann::json json = nlohmann::json::parse(response);
            if(json.contains("error")) {
                int rescode = database->execute_params("DELETE FROM mappings WHERE key = ?1", { key.toStdString() });
                if(rescode != SQLITE_OK) neroshop::print("sqlite error: DELETE failed", 1);
                //emit categoryProductCountChanged();//(category_id);
                //emit searchResultsChanged();
                continue; // Key is lost or missing from DHT, skip to next iteration
            }
            
            const auto& response_obj = json["response"];
            assert(response_obj.is_object());
            if (response_obj.contains("value") && response_obj["value"].is_string()) {
                const auto& value = response_obj["value"].get<std::string>();
                nlohmann::json value_obj = nlohmann::json::parse(value);
                assert(value_obj.is_object());//std::cout << value_obj.dump(4) << "\n";
                std::string metadata = value_obj["metadata"].get<std::string>();
                if (metadata != "listing") { std::cerr << "Invalid metadata. \"listing\" expected, got \"" << metadata << "\" instead\n"; continue; }
                listing.insert("key", key);
                listing.insert("listing_uuid", QString::fromStdString(value_obj["id"].get<std::string>()));
                listing.insert("seller_id", QString::fromStdString(value_obj["seller_id"].get<std::string>()));
                listing.insert("quantity", value_obj["quantity"].get<int>());
                listing.insert("price", value_obj["price"].get<double>());
                listing.insert("currency", QString::fromStdString(value_obj["currency"].get<std::string>()));
                listing.insert("condition", QString::fromStdString(value_obj["condition"].get<std::string>()));
                if(value_obj.contains("location") && value_obj["location"].is_string()) {
                    listing.insert("location", QString::fromStdString(value_obj["location"].get<std::string>()));
                }
                listing.insert("date", QString::fromStdString(value_obj["date"].get<std::string>()));
                assert(value_obj["product"].is_object());
                const auto& product_obj = value_obj["product"];
                listing.insert("product_uuid", QString::fromStdString(product_obj["id"].get<std::string>()));
                listing.insert("product_name", QString::fromStdString(product_obj["name"].get<std::string>()));
                listing.insert("product_description", QString::fromStdString(product_obj["description"].get<std::string>()));
                // product category and subcategories
                std::string category = product_obj["category"].get<std::string>();//int product_category_id = get_category_id_by_name(product_category_name);
                product_categories.append(QString::fromStdString(category));
                if (product_obj.contains("subcategories") && product_obj["subcategories"].is_array()) {
                    const auto& subcategories_array = product_obj["subcategories"];
                    for (const auto& subcategory : subcategories_array) {
                        if (subcategory.is_string()) {
                            product_categories.append(QString::fromStdString(subcategory.get<std::string>()));
                        }
                    }
                    listing.insert("product_categories", product_categories);
                }
                //listing.insert("", QString::fromStdString(product_obj[""].get<std::string>()));
                // product attributes
                if (product_obj.contains("attributes") && product_obj["attributes"].is_array()) {
                    const auto& attributes_array = product_obj["attributes"];
                    for (const auto& attribute : attributes_array) {
                        if (attribute.is_object() && attribute.contains("weight")) { // attributes is an array of objects
                            double weight = attribute["weight"].get<double>();
                            listing.insert("product_weight", weight);
                        }
                    }
                }
                // product images
                if (product_obj.contains("images") && product_obj["images"].is_array()) {
                    const auto& images_array = product_obj["images"];
                    for (const auto& image : images_array) {
                        if (image.contains("name") && image.contains("id")) {
                            const auto& image_name = image["name"].get<std::string>();
                            const auto& image_id = image["id"].get<int>();
                            QVariantMap image_map;
                            image_map.insert("name", QString::fromStdString(image_name));
                            image_map.insert("id", image_id);
                            product_images.append(image_map);
                        }
                    }
                    listing.insert("product_images", product_images);
                }
                // product thumbnail
                if (product_obj.contains("thumbnail") && product_obj["thumbnail"].is_string()) {
                    listing.insert("product_thumbnail", QString::fromStdString(product_obj["thumbnail"].get<std::string>()));
                }
                // Skip products with illegal categories/subcategories
                if (hide_illicit_items) {
                    if(isIllicitItem(listing)) {
                        continue;
                    }
                }
            }
            catalog.append(listing);
        }
    }
    
    sqlite3_finalize(stmt);
    
    switch(sorting) {
        case SortNone:
            // Code for sorting by none - do nothing
            break;
        case SortByCategory:
            // Code for sorting by category - unavailable. Use getListingsByCategory() instead
            break;
        case SortByMostRecent:
            // Perform the sorting operation on the catalog based on the "most recent" criteria
            std::sort(catalog.begin(), catalog.end(), [](const QVariant& a, const QVariant& b) {
                QVariantMap listingA = a.toMap();
                QVariantMap listingB = b.toMap();
                QString dateA = listingA["date"].toString();
                QString dateB = listingB["date"].toString();
                
                // Convert 'Z' to UTC+0 offset
                if (dateA.endsWith("Z")) {
                    dateA.replace(dateA.length() - 1, 1, "+00:00");
                }
                if (dateB.endsWith("Z")) {
                    dateB.replace(dateB.length() - 1, 1, "+00:00");
                }
                
                QDateTime dateTimeA = QDateTime::fromString(dateA, Qt::ISODateWithMs);
                QDateTime dateTimeB = QDateTime::fromString(dateB, Qt::ISODateWithMs);

                return dateTimeA > dateTimeB;
            });
            break;
        case SortByOldest:
            std::sort(catalog.begin(), catalog.end(), [](const QVariant& a, const QVariant& b) {
                QVariantMap listingA = a.toMap();
                QVariantMap listingB = b.toMap();
                QString dateA = listingA["date"].toString();
                QString dateB = listingB["date"].toString();
                
                // Convert 'Z' to UTC+0 offset
                if (dateA.endsWith("Z")) {
                    dateA.replace(dateA.length() - 1, 1, "+00:00");
                }
                if (dateB.endsWith("Z")) {
                    dateB.replace(dateB.length() - 1, 1, "+00:00");
                }
                
                QDateTime dateTimeA = QDateTime::fromString(dateA, Qt::ISODateWithMs);
                QDateTime dateTimeB = QDateTime::fromString(dateB, Qt::ISODateWithMs);

                return dateTimeA < dateTimeB;
            });
            break;
        case SortByAlphabeticalOrder:
            // Sort the catalog list by product name (alphabetically)
            std::sort(catalog.begin(), catalog.end(), [](const QVariant& listing1, const QVariant& listing2) {
                QString productName1 = listing1.toMap()["product_name"].toString();
                QString productName2 = listing2.toMap()["product_name"].toString();
                return productName1 < productName2;
            });
            break;
        case SortByPriceLowest:
            // Perform the sorting operation on the catalog based on the "price lowest" criteria
            std::sort(catalog.begin(), catalog.end(), [](const QVariant& a, const QVariant& b) {
                QVariantMap listingA = a.toMap();
                QVariantMap listingB = b.toMap();
                return listingA["price"].toDouble() < listingB["price"].toDouble();
            });
            break;
        case SortByPriceHighest:
            // Perform the sorting operation on the catalog based on the "price highest" criteria
            std::sort(catalog.begin(), catalog.end(), [](const QVariant& a, const QVariant& b) {
                QVariantMap listingA = a.toMap();
                QVariantMap listingB = b.toMap();
                return listingA["price"].toDouble() > listingB["price"].toDouble();
            });
            break;
        case SortByMostFavorited:
            // Code for sorting by most favorited
            break;
        case SortByMostSales:
            // Code for sorting by most sales
            break;
        default:
            // Code for handling unknown sorting value - do nothing
            break;
    }

    return catalog;    
}
//----------------------------------------------------------------
QVariantList neroshop::Backend::getListingsByCategory(int category_id, bool hide_illicit_items) {
    // Transition from Sqlite to DHT:
    Client * client = Client::get_main_client();
    db::Sqlite3 * database = neroshop::get_database();
    if(!database) throw std::runtime_error("database is NULL");
    
    std::string command = "SELECT DISTINCT key FROM mappings WHERE search_term MATCH ? AND content MATCH 'listing';";
    sqlite3_stmt * stmt = nullptr;
    // Prepare (compile) statement
    if(sqlite3_prepare_v2(database->get_handle(), command.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        neroshop::print("sqlite3_prepare_v2: " + std::string(sqlite3_errmsg(database->get_handle())), 1);
        return {};
    }
    //-------------------------------------------------------
    std::string category = get_category_name_by_id(category_id);
    
    // Replace ampersands with wildcard (*)
    std::replace(category.begin(), category.end(), '&', '*');
    // Add double quotes around the category for phrase matching
    category = "\"" + category + "\"";
    //-------------------------------------------------------
    // Bind value to parameter arguments
    if(sqlite3_bind_text(stmt, 1, category.c_str(), category.length(), SQLITE_STATIC) != SQLITE_OK) {
        neroshop::print("sqlite3_bind_text: " + std::string(sqlite3_errmsg(database->get_handle())), 1);
        sqlite3_finalize(stmt);
        return {};//database->execute("ROLLBACK;"); return {};
    }        
    //-------------------------------------------------------
    // Check whether the prepared statement returns no data (for example an UPDATE)
    if(sqlite3_column_count(stmt) == 0) {
        neroshop::print("No data found. Be sure to use an appropriate SELECT statement", 1);
        return {};
    }
    
    QVariantList catalog;
    // Get all table values row by row
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        QVariantMap listing; // Create an object for each row
        QVariantList product_images;
        QStringList product_categories;

        for(int i = 0; i < sqlite3_column_count(stmt); i++) {
            std::string column_value = (sqlite3_column_text(stmt, i) == nullptr) ? "NULL" : reinterpret_cast<const char *>(sqlite3_column_text(stmt, i));//std::cout << column_value  << " (" << i << ")" << std::endl;
            if(column_value == "NULL") continue; // Skip invalid columns
            QString key = QString::fromStdString(column_value);
            // Get the value of the corresponding key from the DHT
            std::string response;
            client->get(key.toStdString(), response); // TODO: error handling
            std::cout << "Received response (get): " << response << "\n";
            // Parse the response
            nlohmann::json json = nlohmann::json::parse(response);
            if(json.contains("error")) {
                int rescode = database->execute_params("DELETE FROM mappings WHERE key = ?1", { key.toStdString() });
                if(rescode != SQLITE_OK) neroshop::print("sqlite error: DELETE failed", 1);
                //emit categoryProductCountChanged();//(category_id);
                //emit searchResultsChanged();
                continue; // Key is lost or missing from DHT, skip to next iteration
            }
            
            const auto& response_obj = json["response"];
            assert(response_obj.is_object());
            if (response_obj.contains("value") && response_obj["value"].is_string()) {
                const auto& value = response_obj["value"].get<std::string>();
                nlohmann::json value_obj = nlohmann::json::parse(value);
                assert(value_obj.is_object());//std::cout << value_obj.dump(4) << "\n";
                std::string metadata = value_obj["metadata"].get<std::string>();
                if (metadata != "listing") { std::cerr << "Invalid metadata. \"listing\" expected, got \"" << metadata << "\" instead\n"; continue; }
                listing.insert("key", key);
                listing.insert("listing_uuid", QString::fromStdString(value_obj["id"].get<std::string>()));
                listing.insert("seller_id", QString::fromStdString(value_obj["seller_id"].get<std::string>()));
                listing.insert("quantity", value_obj["quantity"].get<int>());
                listing.insert("price", value_obj["price"].get<double>());
                listing.insert("currency", QString::fromStdString(value_obj["currency"].get<std::string>()));
                listing.insert("condition", QString::fromStdString(value_obj["condition"].get<std::string>()));
                if(value_obj.contains("location") && value_obj["location"].is_string()) {
                    listing.insert("location", QString::fromStdString(value_obj["location"].get<std::string>()));
                }
                listing.insert("date", QString::fromStdString(value_obj["date"].get<std::string>()));
                assert(value_obj["product"].is_object());
                const auto& product_obj = value_obj["product"];
                listing.insert("product_uuid", QString::fromStdString(product_obj["id"].get<std::string>()));
                listing.insert("product_name", QString::fromStdString(product_obj["name"].get<std::string>()));
                listing.insert("product_description", QString::fromStdString(product_obj["description"].get<std::string>()));
                // product category and subcategories
                std::string category = product_obj["category"].get<std::string>();
                product_categories.append(QString::fromStdString(category));
                if (product_obj.contains("subcategories") && product_obj["subcategories"].is_array()) {
                    const auto& subcategories_array = product_obj["subcategories"];
                    for (const auto& subcategory : subcategories_array) {
                        if (subcategory.is_string()) {
                            product_categories.append(QString::fromStdString(subcategory.get<std::string>()));
                        }
                    }
                    listing.insert("product_categories", product_categories);
                }
                //listing.insert("weight", QString::fromStdString(product_obj[""].get<std::string>()));
                //listing.insert("other_attr", QString::fromStdString(product_obj[""].get<std::string>()));
                //listing.insert("code", QString::fromStdString(product_obj[""].get<std::string>()));
                //listing.insert("tags", QString::fromStdString(product_obj[""].get<std::string>()));
                if (product_obj.contains("images") && product_obj["images"].is_array()) {
                    const auto& images_array = product_obj["images"];
                    for (const auto& image : images_array) {
                        if (image.contains("name") && image.contains("id")) {
                            const auto& image_name = image["name"].get<std::string>();
                            const auto& image_id = image["id"].get<int>();//source,data, etc.
                            
                            QVariantMap image_map;
                            image_map.insert("name", QString::fromStdString(image_name));
                            image_map.insert("id", image_id);
                            product_images.append(image_map);
                        }
                    }
                    listing.insert("product_images", product_images);
                }
                // Skip products with illegal categories/subcategories
                if (hide_illicit_items) {
                    if(isIllicitItem(listing)) {
                        continue;
                    }
                }
            }
            catalog.append(listing);
        }
    }
    
    sqlite3_finalize(stmt);

    return catalog;
}
//----------------------------------------------------------------
QVariantList neroshop::Backend::getListingsByMostRecentLimit(int limit, bool hide_illicit_items) {
    auto catalog = getListings(SortByMostRecent, hide_illicit_items);
    if (catalog.size() > limit) {
        catalog = catalog.mid(0, limit);
    }
    return catalog;
}
//----------------------------------------------------------------
//----------------------------------------------------------------
bool neroshop::Backend::isIllicitItem(const QVariantMap& listing_obj) {
    std::string illegal_category_name = "Illicit Goods & Services";
    
    if (!listing_obj.contains("product_categories")) {
        std::cerr << "No product categories found\n";
        return false;
    }
    
    QStringList product_categories = listing_obj["product_categories"].toStringList();
    if(product_categories.contains(QString::fromStdString(illegal_category_name))) {
        std::cout << listing_obj["product_name"].toString().toStdString() << " contains illegal content so it has been excluded from listings" << "\n";
        return true;
    }
    return false;
}
//----------------------------------------------------------------
//----------------------------------------------------------------
void neroshop::Backend::createOrder(UserController * user_controller, const QString& shipping_address) {
    user_controller->createOrder(shipping_address);
}
//----------------------------------------------------------------
//----------------------------------------------------------------
QVariantList neroshop::Backend::getNodeListDefault(const QString& coin) const {
    QVariantList node_list;
    std::string network_type = neroshop::Script::get_string(neroshop::lua_state, "monero.network_type");
    std::vector<std::string> node_table = neroshop::Script::get_table_string(neroshop::lua_state, coin.toStdString() + ".nodes." + network_type); // Get monero nodes from settings.lua////std::cout << "lua_query: " << coin.toStdString() + ".nodes." + network_type << std::endl;
    for(auto strings : node_table) {
        node_list << QString::fromStdString(strings);
    }
    return node_list;
}
//----------------------------------------------------------------
QVariantList neroshop::Backend::getNodeList(const QString& coin) const {
    const QUrl url(QStringLiteral("https://monero.fail/health.json"));
    QVariantList node_list;
    QString coin_lower = coin.toLower(); // make coin name lowercase
    
    QNetworkAccessManager manager;
    QEventLoop loop;
    QObject::connect(&manager, &QNetworkAccessManager::finished, &loop, &QEventLoop::quit);

    auto reply = manager.get(QNetworkRequest(url));
    loop.exec();
    QJsonParseError error;
    const auto json_doc = QJsonDocument::fromJson(reply->readAll(), &error);
    // Use fallback monero node list if we fail to get the nodes from the url
    if (error.error != QJsonParseError::NoError) {
        neroshop::print("Error reading json from " + url.toString().toStdString() + "\nUsing default nodes as fallback", 2);
        return getNodeListDefault(coin_lower);
    }
    // Get monero nodes from the JSON
    QJsonObject root_obj = json_doc.object(); // {}
    QJsonObject coin_obj = root_obj.value(coin_lower).toObject(); // "monero": {} // "wownero": {}
    QJsonObject clearnet_obj = coin_obj.value("clear").toObject(); // "clear": {} // "onion": {}, "web_compatible": {}
    // Loop through monero nodes (clearnet)
    foreach(const QString& key, clearnet_obj.keys()) {//for (const auto monero_nodes : clearnet_obj) {
        QJsonObject monero_node_obj = clearnet_obj.value(key).toObject();//QJsonObject monero_node_obj = monero_nodes.toObject();
        QVariantMap node_object; // Create an object for each row
        if(key.contains("38081") || key.contains("38089")) { // Temporarily fetch only stagenet nodes (TODO: change to mainnet port upon release)
        node_object.insert("address", key);
        node_object.insert("available", monero_node_obj.value("available").toBool());//std::cout << "available: " << monero_node_obj.value("available").toBool() << "\n";
        ////node_object.insert("", );//////std::cout << ": " << monero_node_obj.value("checks").toArray() << "\n";
        node_object.insert("datetime_checked", monero_node_obj.value("datetime_checked").toString());//std::cout << "datetime_checked: " << monero_node_obj.value("datetime_checked").toString().toStdString() << "\n";
        node_object.insert("datetime_entered", monero_node_obj.value("datetime_entered").toString());//std::cout << "datetime_entered: " << monero_node_obj.value("datetime_entered").toString().toStdString() << "\n";
        node_object.insert("datetime_failed", monero_node_obj.value("datetime_failed").toString());//std::cout << "datetime_failed: " << monero_node_obj.value("datetime_failed").toString().toStdString() << "\n";
        node_object.insert("last_height", monero_node_obj.value("last_height").toInt());//std::cout << "last_height: " << monero_node_obj.value("last_height").toInt() << "\n";
        node_list.append(node_object); // Add node object to the node list
        }
    }
    return node_list;
}
//----------------------------------------------------------------
// Todo: use QProcess to check if monero daemon is running
bool neroshop::Backend::isWalletDaemonRunning() const {
    /*int monerod = Process::get_process_by_name("monerod");
    if(monerod == -1) { return false; }
    std::cout << "\033[1;90;49m" << "monerod is running (ID:" << monerod << ")\033[0m" << std::endl; 
    return true;*/
    
    #ifdef Q_OS_WIN
    QString program = "monerod.exe";
    #else
    QString program = "monerod";
    #endif
    
    QProcess process;
    process.start("pgrep", QStringList() << program); // specific to Linux-based systems
    process.waitForFinished();

    if(process.exitCode() == 0) std::cout << "\033[1;90;49m" << program.toStdString() << " was already running in the background\033[0m" << std::endl;
    return process.exitCode() == 0;
}
//----------------------------------------------------------------
//----------------------------------------------------------------
// TODO: replace function return type with enum
QVariantList neroshop::Backend::validateDisplayName(const QString& display_name) const {
    // username (will appear only in lower-case letters within the app)
    std::string username = display_name.toStdString();
    // Empty display names are acceptable
    if(display_name.isEmpty()) return { true, "" };

    if(!neroshop::string_tools::is_valid_username(username)) {
        std::string default_message = "Invalid username: " + username;
        neroshop::print(default_message, 1);
        if (username.length() < 2) {
            std::string message = "must be at least 2 characters in length";
            return { false, QString::fromStdString(message) };
        }
        if (username.length() > 30) {
            std::string message = "cannot exceed 30 characters in length";
            return { false, QString::fromStdString(message) };
        }
        if (std::regex_search(username, std::regex("\\s"))) {
            std::string message = "cannot contain spaces\n";
            return { false, QString::fromStdString(message) };
        }
        if (!std::regex_search(username, std::regex("^[a-zA-Z]"))) {
            std::string message = "must begin with a letter (cannot start with a symbol or number)";
            return { false, QString::fromStdString(message) };
        }
        if (!std::regex_search(username, std::regex("[a-zA-Z0-9]$"))) {
            std::string message = "must end with a letter or number (cannot end with a symbol)";
            return { false, QString::fromStdString(message) };
        }
        if (std::regex_search(username, std::regex("[^a-zA-Z0-9._-]"))) {
            std::string message = "contains invalid symbol(s) (only '.', '_', and '-' are allowed in between the display name)";
            return { false, QString::fromStdString(message) };
        }
        if (username == "Guest") {
            std::string message = "name \"Guest\" is reserved for guests only and cannot be used by any other user";
            return { false, QString::fromStdString(message) };
        }
        return { false, QString::fromStdString(default_message) };
    }

    return { true, "" };
}
//----------------------------------------------------------------
// TODO: replace function return type with enum
QVariantList neroshop::Backend::registerUser(WalletController* wallet_controller, const QString& display_name, UserController * user_controller, const QString& avatar) {
    // Make sure daemon is connected first
    if(!DaemonManager::isDaemonServerBound()) {
        return { false, "Please wait for the daemon LIPC server to connect first" };
    }
    //---------------------------------------------
    db::Sqlite3 * database = neroshop::get_database();
    if(!database) throw std::runtime_error("database is NULL");
    //---------------------------------------------
    // Validate display name
    auto name_validation_result = validateDisplayName(display_name);
    if(!name_validation_result[0].toBool()) {
        bool boolean_result = name_validation_result[0].toBool();
        QString message_result = name_validation_result[1].toString();
        return { boolean_result, message_result };
    }
    //---------------------------------------------
    // Get wallet primary address and check its validity
    std::string primary_address = wallet_controller->getPrimaryAddress().toStdString();//neroshop::print("Primary address: \033[1;33m" + primary_address + "\033[1;37m\n");
    if(!wallet_controller->getWallet()->is_valid_address(primary_address)) {
        return { false, "Invalid monero address" };
    }
    //---------------------------------------------
    // Generate RSA key pair (this is for sending/receiving encrypted messages)
    std::string config_path = NEROSHOP_DEFAULT_CONFIGURATION_PATH;
    std::string public_key_filename = config_path + "/" + primary_address + ".pub";
    std::string private_key_filename = config_path + "/" + primary_address + ".key";
    EVP_PKEY * pkey = neroshop::crypto::rsa_generate_keys_get();
    if(pkey == nullptr) {
        return { false, "Failed to generate RSA key pair" };
    }
    // Get a copy of the public key
    std::string public_key = neroshop::crypto::rsa_get_public_key(pkey);
    std::string private_key = neroshop::crypto::rsa_get_private_key(pkey);
    // Save the key pair to disk
    if(!neroshop::crypto::rsa_save_keys(pkey, public_key_filename, private_key_filename)) {
        return { false, "Failed to save RSA key pair" };
    }
    //---------------------------------------------
    // Note: Multiple users can have the same display_name as long as the id is unique!
    // initialize user obj
    std::unique_ptr<neroshop::User> seller(neroshop::Seller::on_login(*wallet_controller->getWallet()));
    user_controller->_user = std::move(seller);
    if (user_controller->getUser() == nullptr) {
        return {false, "user is NULL"};
    }
    user_controller->_user->set_name(display_name.toStdString());
    user_controller->_user->set_public_key(public_key);
    user_controller->_user->set_private_key(private_key);
    if(!avatar.isEmpty()) {
        user_controller->_user->upload_avatar(avatar.toStdString()); // will initialize avatar obj
    }
    //---------------------------------------------
    // Store login credentials in DHT
    Client * client = Client::get_main_client();
    // If client is not connect, return error
    if (!client->is_connected()) return { false, "Not connected to daemon LIPC server" };
    // Serialize user object
    auto data = Serializer::serialize(*user_controller->_user);
    std::string key = data.first;
    std::string value = data.second;
    
    // Send put and receive response
    std::string response;
    client->put(key, value, response);
    std::cout << "Received response (put): " << response << "\n";
    //---------------------------------------------
    // Create cart for user
    QString cart_uuid = QUuid::createUuid().toString();
    cart_uuid = cart_uuid.remove("{").remove("}"); // remove brackets
    database->execute_params("INSERT INTO cart (uuid, user_id) VALUES ($1, $2)", { cart_uuid.toStdString(), user_controller->_user->get_id() });
    //---------------------------------------------
    emit user_controller->userChanged();
    emit user_controller->userLogged();
    // temp - remove soon
    //user_controller->rateItem("8e12c9e7-7017-4bd0-95fe-9abbcd82c1ff", 3, "This product is aiight");
    //user_controller->rateSeller("5AQFFwoqqBWMYCh6b2V6RyEKZS5ozqfCP1uLnf5FTHiqjk9qyDGoj62Vva2jz71nFGPsDgXAfv2q4GGaWyV2EQ2xTfgFPCw", 1, "This seller rocks");
    // Display registration message
    neroshop::print(((!display_name.isEmpty()) ? "Welcome to neroshop, " : "Welcome to neroshop") + display_name.toStdString(), 4);
    return { true, QString::fromStdString(key) };
}
//----------------------------------------------------------------
int neroshop::Backend::loginWithWalletFile(WalletController* wallet_controller, const QString& path, const QString& password, UserController * user_controller) { 
    db::Sqlite3 * database = neroshop::get_database();
    if(!database) throw std::runtime_error("database is NULL");
    
    // Make sure daemon is connected first
    if(!DaemonManager::isDaemonServerBound()) {
        neroshop::print("Please wait for the daemon LIPC server to connect first", 1);
        return static_cast<int>(EnumWrapper::LoginError::DaemonIsNotConnected);
    }    
    // Open wallet file
    std::packaged_task<int(void)> open_wallet_task([wallet_controller, path, password]() -> int {
        int wallet_error = wallet_controller->open(path, password);
        if(wallet_error != 0) {
            if(wallet_error == static_cast<int>(WalletError::WrongPassword))
                return static_cast<int>(EnumWrapper::LoginError::WrongPassword);
            if(wallet_error == static_cast<int>(WalletError::IsOpenedByAnotherProgram))
                return static_cast<int>(EnumWrapper::LoginError::WalletIsOpenedByAnotherProgram);
            if(wallet_error == static_cast<int>(WalletError::DoesNotExist))
                return static_cast<int>(EnumWrapper::LoginError::WalletDoesNotExist);
            if(wallet_error == static_cast<int>(WalletError::BadNetworkType))
                return static_cast<int>(EnumWrapper::LoginError::WalletBadNetworkType);
            if(wallet_error == static_cast<int>(WalletError::IsNotOpened))
                return static_cast<int>(EnumWrapper::LoginError::WalletIsNotOpened);
        }
        return static_cast<int>(EnumWrapper::LoginError::Ok);
    });
    std::future<int> future_result = open_wallet_task.get_future();
    // move the task (function) to a separate thread to prevent blocking of the main thread
    std::thread worker(std::move(open_wallet_task));
    worker.detach(); // join may block but detach won't
    int login_error = future_result.get();
    if(login_error != 0) return login_error;
    // Get the primary address
    std::string primary_address = wallet_controller->getPrimaryAddress().toStdString();
    //----------------------------------------
    // Check database to see if user key (hash of primary address) exists
    bool user_found = database->get_integer_params("SELECT EXISTS(SELECT * FROM mappings WHERE search_term = ?1 AND content = 'account')", { primary_address });
    // If user key is not found in the database, then create one. This is like registering for an account
    if(!user_found) {
        // In reality, this function will return false if user key is not registered in the database
        neroshop::print("Account not found in database. Please try again or register", 1);
        wallet_controller->close();
        return static_cast<int>(EnumWrapper::LoginError::UserNotFound);
    }
    // Get the account DHT key
    std::string account_key = database->get_text_params("SELECT key FROM mappings WHERE search_term = ?1 AND content = 'account'", { primary_address });
    // Save user information in memory
    std::string display_name = database->get_text_params("SELECT search_term FROM mappings WHERE key = ?1 AND LENGTH(search_term) <= 30 AND content = 'account'", { account_key });
    std::unique_ptr<neroshop::User> seller(neroshop::Seller::on_login(*wallet_controller->getWallet()));
    user_controller->_user = std::move(seller);
    if(user_controller->getUser() == nullptr) {
        return static_cast<int>(EnumWrapper::LoginError::UserIsNullPointer);
    }
    //----------------------------------------
    // Load RSA keys from file
    std::string config_path = NEROSHOP_DEFAULT_CONFIGURATION_PATH;
    std::string public_key_path = config_path + "/" + primary_address + ".pub";
    std::string private_key_path = config_path + "/" + primary_address + ".key";
    //----------------------------------------
    // Load public_key
    std::ifstream public_key_file(public_key_path);
    if (!public_key_file) {
        // Handle file open error
        throw std::runtime_error("Failed to open public key file: " + public_key_path);
    }

    std::ostringstream buffer0;
    buffer0 << public_key_file.rdbuf();
    std::string public_key = buffer0.str();
    //----------------------------------------
    // Load private_key
    std::ifstream private_key_file(private_key_path);
    if (!private_key_file) {
        // Handle file open error
        throw std::runtime_error("Failed to open private key file: " + private_key_path);
    }

    std::ostringstream buffer;
    buffer << private_key_file.rdbuf();
    std::string private_key = buffer.str();    
    //----------------------------------------
    // Set RSA private key
    user_controller->_user->set_public_key(public_key);
    user_controller->_user->set_private_key(private_key);
    //----------------------------------------
    emit user_controller->userChanged();
    emit user_controller->userLogged();
    // temp - remove soon
    //getSellerRatings(wallet_controller->getPrimaryAddress());
    //getSellerRatingsCount("5AncSFWauoN8bfA68uYpWJRM8fFxEqztuhXSGkeQn5Xd9yU6XqJPW7cZmtYETUAjTK1fCfYQX1CP3Dnmy5a8eUSM5n3C6aL");
    //getSellerReputation("5AncSFWauoN8bfA68uYpWJRM8fFxEqztuhXSGkeQn5Xd9yU6XqJPW7cZmtYETUAjTK1fCfYQX1CP3Dnmy5a8eUSM5n3C6aL");
    //getProductRatings("2b715653-da61-4ea0-8b5a-ad2754d78ba1");
    /*getProductStarCount("2b715653-da61-4ea0-8b5a-ad2754d78ba1");
    getProductStarCount("2b715653-da61-4ea0-8b5a-ad2754d78ba1", 5);
    getProductStarCount("2b715653-da61-4ea0-8b5a-ad2754d78ba1", 4);
    getProductStarCount("2b715653-da61-4ea0-8b5a-ad2754d78ba1", 3);
    getProductStarCount("2b715653-da61-4ea0-8b5a-ad2754d78ba1", 2);
    getProductStarCount("2b715653-da61-4ea0-8b5a-ad2754d78ba1", 1);
    getProductAverageStars("2b715653-da61-4ea0-8b5a-ad2754d78ba1");*/
    //user_controller->createOrder("12 Robot Dr. Boston MA 02115");
    // Display message
    neroshop::print("Welcome back, user " + ((!display_name.empty()) ? (display_name + " (id: " + primary_address + ")") : primary_address), 4);
    return static_cast<int>(EnumWrapper::LoginError::Ok);
}
//----------------------------------------------------------------
int neroshop::Backend::loginWithMnemonic(WalletController* wallet_controller, const QString& mnemonic, UserController * user_controller) {
    db::Sqlite3 * database = neroshop::get_database();
    if(!database) throw std::runtime_error("database is NULL");
    // Initialize monero wallet with existing wallet mnemonic
    if(!wallet_controller->restoreFromSeed(mnemonic)) {
        throw std::runtime_error("Invalid mnemonic or wallet network type");
        return false;    
    }
    // Get the primary address
    std::string primary_address = wallet_controller->getPrimaryAddress().toStdString();
    // Check database to see if user key (hash of primary address) exists
    bool user_key_found = database->get_integer_params("SELECT EXISTS(SELECT * FROM users WHERE monero_address = $1)", { primary_address });
    // If user key is not found in the database, then create one. This is like registering for an account
    if(!user_key_found) {
        // In reality, this function will return false if user key is not registered in the database
        neroshop::print("user key not found in database. Please try again or register", 1);
        wallet_controller->close();
        return false;
    }
    // Save user information in memory
    int user_id = database->get_integer_params("SELECT id FROM users WHERE monero_address = $1", { primary_address });
    // Display message
    std::string display_name = database->get_text_params("SELECT name FROM users WHERE monero_address = $1", { primary_address });
    neroshop::print("Welcome back, user " + ((!display_name.empty()) ? (display_name + " (id: " + primary_address + ")") : primary_address), 4);
    return true;
}
//----------------------------------------------------------------
int neroshop::Backend::loginWithKeys(WalletController* wallet_controller, UserController * user_controller) {
/*
    db::Sqlite3 * database = neroshop::get_database();
    if(!database) throw std::runtime_error("database is NULL");
    // Get the wallet from the wallet controller
    neroshop::Wallet * wallet = wallet_controller->getWallet();
    // Initialize monero wallet with existing wallet mnemonic
    std::string primary_address;
    std::string secret_view_key;
    std::string secret_spend_key;
    std::cout << "Please enter your primary address:\n";
    std::getline(std::cin, primary_address);
    std::cout << "Please enter your secret view key:\n";
    std::getline(std::cin, secret_view_key);
    std::cout << "Please enter your secret spend key (optional):\n";
    std::getline(std::cin, secret_spend_key);
    // todo: allow user to specify a custom location for the wallet keyfile or use a default location
    wallet_controller->restoreFromKeys(primary_address, secret_view_key, secret_spend_key);
    // Get the hash of the primary address
    std::string user_auth_key;// = neroshop::algo::sha256(primary_address);
    ////Validator::generate_sha256_hash(primary_address, user_auth_key); // temp
    neroshop::print("Primary address: \033[1;33m" + primary_address + "\033[1;37m\nSHA256 hash: " + user_auth_key);
    //$ echo -n "528qdm2pXnYYesCy5VdmBneWeaSZutEijFVAKjpVHeVd4unsCSM55CjgViQsK9WFNHK1eZgcCuZ3fRqYpzKDokqSKp4yp38" | sha256sum
    // Check database to see if user key (hash of primary address) exists
    bool user_key_found = database->get_integer_params("SELECT EXISTS(SELECT * FROM users WHERE key = $1)", { user_auth_key });
    // If user key is not found in the database, then create one. This is like registering for an account
    if(!user_key_found) {
        // In reality, this function will return false if user key is not registered in the database
        neroshop::print("user key not found in database. Please try again or register", 1);
        wallet_controller->close();
        return false;
    }
    // Save user information in memory
    int user_id = database->get_integer_params("SELECT id FROM users WHERE key = $1", { user_auth_key });
    // Display message
    std::string display_name = database->get_text_params("SELECT name FROM users WHERE monero_address = $1", { primary_address });
    neroshop::print("Welcome back, user " + ((!display_name.empty()) ? (display_name + " (id: " + primary_address + ")") : primary_address), 4);
    return true;
*/
    return false;
}
//----------------------------------------------------------------
int neroshop::Backend::loginWithHW(WalletController* wallet_controller, UserController * user_controller) {
    return false;
}
//----------------------------------------------------------------
//----------------------------------------------------------------
QVariantMap neroshop::Backend::getNetworkStatus() const {
    // Make sure daemon is connected first
    if(!DaemonManager::isDaemonServerBound()) {
        return {};
    }
    
    Client * client = Client::get_main_client();
    
    // Get network status from local node in IPC mode
    std::string response;
    client->get("status", response); //std::cout << "Received response (get): " << response << "\n";
    
    // Parse the response
    nlohmann::json json = nlohmann::json::parse(response);
    if(json.contains("error")) {
        return {};
    }
    
    QVariantMap network_status;
            
    const auto& response_obj = json["response"];
    assert(response_obj.is_object());
    if (response_obj.contains("connected_peers") && response_obj["connected_peers"].is_number_integer()) {
        int connected_peers = response_obj["connected_peers"].get<int>();
        network_status["connected_peers"] = connected_peers;
    }
    
    if (response_obj.contains("active_peers") && response_obj["active_peers"].is_number_integer()) {
        int active_peers = response_obj["active_peers"].get<int>();
        network_status["active_peers"] = active_peers;
    }
    
    if (response_obj.contains("idle_peers") && response_obj["idle_peers"].is_number_integer()) {
        int idle_peers = response_obj["idle_peers"].get<int>();
        network_status["idle_peers"] = idle_peers;
    }
    
    return network_status;
}
//----------------------------------------------------------------

