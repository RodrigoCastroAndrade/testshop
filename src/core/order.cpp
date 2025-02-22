#include "order.hpp"

#include "cart.hpp"
#include "price/currency_converter.hpp" // currency converter, enums.hpp
#include "settings.hpp" // neroshop::lua_state
#include "database/database.hpp"
#include "tools/logger.hpp"
#include "tools/tools.hpp" // neroshop::uuid::generate()
#include "protocol/p2p/serializer.hpp"
#include "protocol/transport/client.hpp"
#include "tools/timestamp.hpp"

#include <unordered_set>
////////////////////
neroshop::Order::Order() : status(OrderStatus::New), subtotal(0.00), discount(0.00), shipping_cost(0.00), total(0.00), 
    payment_option(PaymentOption::Escrow), payment_coin(PaymentCoin::Monero), delivery_option(DeliveryOption::Delivery)
{}

neroshop::Order::Order(const std::string& id, const std::string& date, OrderStatus status, const std::string& customer_id,
	    double subtotal, double discount, double shipping_cost, double total, PaymentOption payment_option,
	    PaymentCoin payment_coin, DeliveryOption delivery_option, const std::string& notes,
	    const std::vector<std::tuple<std::string, int, std::string>>& items)
	: id(id), date(date), status(status), customer_id(customer_id), subtotal(subtotal), discount(discount),
	  shipping_cost(shipping_cost), total(total), payment_option(payment_option), payment_coin(payment_coin),
	  delivery_option(delivery_option), notes(notes), items(items)
{}

neroshop::Order::~Order() {
#ifdef NEROSHOP_DEBUG
    std::cout << "order deleted\n";
#endif    
}
////////////////////
////////////////////
static nlohmann::json get_listing_object(const std::string& listing_key) {
    neroshop::Client * client = neroshop::Client::get_main_client();
    
    neroshop::db::Sqlite3 * database = neroshop::get_database();
    if(!database) throw std::runtime_error("database is NULL");
    // Get the value of the corresponding key from the DHT
    std::string response;
    client->get(listing_key, response);
    std::cout << "Received response (get): " << response << "\n";
    // Parse the response
    nlohmann::json json = nlohmann::json::parse(response);
    if(json.contains("error")) {
        std::cout << "get_available_stock: listing key is lost or missing from DHT\n";
        int rescode = database->execute_params("DELETE FROM mappings WHERE key = ?1", { listing_key });
        if(rescode != SQLITE_OK) neroshop::print("sqlite error: DELETE failed", 1);
        return nlohmann::json(); // Key is lost or missing from DHT
    }
    
    const auto& response_obj = json["response"];
    assert(response_obj.is_object());
    if (response_obj.contains("value") && response_obj["value"].is_string()) {
        const auto& value = response_obj["value"].get<std::string>();
        nlohmann::json value_obj = nlohmann::json::parse(value);
        assert(value_obj.is_object());//std::cout << value_obj.dump(4) << "\n";
        std::string metadata = value_obj["metadata"].get<std::string>();
        if (metadata != "listing") { std::cerr << "Invalid metadata. \"listing\" expected, got \"" << metadata << "\" instead\n"; return nlohmann::json(); }
        return value_obj;
    }
    return nlohmann::json(); // Return an empty JSON object if "value" is not found or not a string
}
////////////////////
void neroshop::Order::create_order(const neroshop::Cart& cart, const std::string& shipping_address) {
    // Transition from Sqlite to DHT:
    if(cart.is_empty()) {
        neroshop::print("You are unable to place an order because your cart is empty", 1);
        return;
    }
    //----------------------------------------------------------------------------
    std::vector<std::tuple<std::string, int, std::string>> order_items;
    double subtotal = 0.00, discount = 0.00, shipping_cost = 0.00, total = 0.00;
    std::string currency = "USD"; // default
    std::unordered_map<std::string, nlohmann::json> listing_cache;
    std::string first_seller_id;
    //----------------------------------------------------------------------------
    for (const auto& [listing_key, listing_qty, seller_id] : cart.contents) {
        nlohmann::json listing_obj = get_listing_object(listing_key);
        if(listing_obj.contains("metadata")) {
            if(!listing_obj.contains("metadata")) { std::cout << "No metadata found"; return; }
            std::string metadata = listing_obj["metadata"].get<std::string>();
            if(metadata != "listing") { neroshop::print("Invalid metadata\n", 1); return; }
            listing_cache.insert({listing_key, listing_obj});
            std::string product_name = listing_obj["product"]["name"].get<std::string>();
        
            // Make sure there is a seller for the item
            if(seller_id.empty()) {
                neroshop::print("No seller found for the following item: " + product_name, 1);
                // TODO: Remove this item from cart
                return;
            }
            
            // Prevent customer from purchasing their own products
            if(cart.owner_id == seller_id) {
                neroshop::print("You cannot order from yourself", 1);
                return;
            }
            
            // Save the seller ID of the first item in the cart
            if (first_seller_id.empty()) {
                first_seller_id = seller_id;
            }
            // Compare the seller ID with the seller ID of the first item
            if (seller_id != first_seller_id) {
                ////neroshop::print("You cannot order items from different sellers in a single order", 1);
                // TODO: Switch to batch mode (create order in batches)
                std::cout << "Order contains items from different sellers.\n\033[1;35mSwitching to batch mode\033[0m\n";
                create_order_batch(cart, shipping_address);
                return;
            }
            
            // Get currency that product is priced in
            currency = listing_obj["currency"].get<std::string>();
            
            // Get sales price
            double sales_price = listing_obj["price"].get<double>();
            
            // Calculate the subtotal (price of all items combined)
            subtotal += listing_qty * sales_price;
            // Deal with the discount later ...
        
            // Check again to see if item is still in stock
            int quantity = listing_obj["quantity"].get<int>();
            if(quantity < listing_qty) {
                neroshop::print("Order request failed (Reason: Quantity has surpassed the stock available for the following item: " + product_name + ")");
                return;
            }
            if(quantity <= 0) {
                neroshop::print("Order request failed (Reason: The following item is out of stock: " + listing_obj["product"]["name"].get<std::string>() + ")");
                return;
            }
            
            // Add each product to order_item of the same order_id
            order_items.emplace_back(listing_key, listing_qty, seller_id);
            // Only the seller can reduce stock quantity of each purchased item
        }
    }
    //----------------------------------------------------------------------------
    // Calculate total
    // Convert price to Monero (XMR)
    double subtotal_monero = neroshop::Converter::convert_to_xmr(subtotal, currency);
    double discount_monero = neroshop::Converter::convert_to_xmr(discount, currency);
    double shipping_cost_monero = neroshop::Converter::convert_to_xmr(shipping_cost, currency);
    total = (subtotal - discount) + shipping_cost;
    double total_monero = neroshop::Converter::convert_to_xmr(total, currency);
    // Convert monero to piconero (for storing in DHT)
    double piconero = 0.000000000001;
    uint64_t subtotal_piconero = subtotal_monero / piconero;
    uint64_t discount_piconero = discount_monero / piconero;
    uint64_t shipping_cost_piconero = shipping_cost_monero / piconero;
    uint64_t total_piconero = total_monero / piconero;
    //----------------------------------------------------------------------------
    // Construct the order
    std::string order_id = neroshop::uuid::generate();
    std::string created_at = neroshop::timestamp::get_current_utc_timestamp();
    auto order_status = OrderStatus::New;
    std::string customer_id = cart.owner_id;
    auto payment_option = PaymentOption::Escrow;
    auto payment_coin = PaymentCoin::Monero;
    auto delivery_option = DeliveryOption::Delivery;
    std::string notes = shipping_address;
    
    /*std::string signature = wallet->sign_message(order_id, monero_message_signature_type::SIGN_WITH_SPEND_KEY);//std::cout << "signature: " << signature << "\n\n";
    
    #ifdef NEROSHOP_DEBUG
    auto result = wallet->verify_message(order_id, signature);
    std::cout << "\033[1mverified: " << (result == 1 ? "\033[32mpass" : "\033[91mfail") << "\033[0m\n";
    assert(result == true);
    #endif*/
    
    Order order = {
        order_id, created_at, order_status, customer_id, 
        subtotal_monero, discount_monero, shipping_cost_monero, total_monero, 
        payment_option, payment_coin, delivery_option, 
        notes, order_items
    };

    auto data = Serializer::serialize(order);
    std::string key = data.first;
    std::string value = data.second;
    std::cout << "key: " << data.first << "\nvalue: " << data.second << "\n";
    
    // Send put request to neighboring nodes (and your node too JIC)
    Client * client = Client::get_main_client();
    std::string response;
    client->put(key, value, response); // TODO: Error handling
    std::cout << "Received response: " << response << "\n";
    //----------------------------------------------------------------------------
    // Print order message
    neroshop::print("Thank you for using neroshop.");
    neroshop::io_write("You have ordered: ");
    for (const auto& [listing_key, listing_qty, seller_id] : cart.contents) {
        nlohmann::json listing_obj = listing_cache.at(listing_key);
        std::string product_name = listing_obj["product"]["name"].get<std::string>();
        std::cout << "\033[0;94m" + product_name << " (x" << listing_qty << ")\033[0m" << std::endl;
    }    
    // Display order details
    nlohmann::json json = nlohmann::json::parse(neroshop::load_json(), nullptr, false); // allow_exceptions set to false
    std::string your_currency = (json.is_discarded()) ? "USD" : json["preferred_currency"].get<std::string>();
    if(your_currency.empty() || !neroshop::Converter::is_supported_currency(your_currency)) your_currency = "USD"; // default //neroshop::Converter::from_xmr(neroshop::Converter::to_xmr(, currency), your_currency);
    neroshop::print("Sit tight as we notify the seller(s) about your order.");
    auto from = Converter::get_currency_enum(currency);
    auto to = Converter::get_currency_enum(your_currency);
    std::cout << "Subtotal: " << std::fixed << std::setprecision(12) << subtotal_monero << " xmr" << std::fixed << std::setprecision(2) << " (" << neroshop::Converter::get_currency_sign(your_currency) << (neroshop::Converter::get_price(from, to) * subtotal) << " " << neroshop::string::upper(your_currency) << ")" <<  std::endl; // (() ? : "")
    if(discount > 0) std::cout << "Discount: -" << std::fixed << std::setprecision(12) << discount_monero << " xmr" << std::fixed << std::setprecision(2) << " (-" << neroshop::Converter::get_currency_sign(your_currency) << (neroshop::Converter::get_price(from, to) * discount) << " " << neroshop::string::upper(your_currency) << ")" <<  std::endl;
    std::cout << "Shipping: " << std::fixed << std::setprecision(12) << shipping_cost_monero << " xmr" << std::fixed << std::setprecision(2) << " (" << neroshop::Converter::get_currency_sign(your_currency) << (neroshop::Converter::get_price(from, to) * shipping_cost) << " " << neroshop::string::upper(your_currency) << ")" <<  std::endl;
    std::cout << "Order total: " << std::fixed << std::setprecision(12) << total_monero << " xmr" << std::fixed << std::setprecision(2) << " (" << neroshop::Converter::get_currency_sign(your_currency) << (neroshop::Converter::get_price(from, to) * total) << " " << neroshop::string::upper(your_currency) << ")" <<  std::endl;
    //std::cout << "Estimated delivery date: " << delivery_date_est << std::endl;    

    // Empty cart after completing order
    const_cast<neroshop::Cart&>(cart).empty();
}
////////////////////
void neroshop::Order::create_order_batch(const neroshop::Cart& cart, const std::string& shipping_address) {
    if(cart.is_empty()) {
        neroshop::print("You are unable to place an order because your cart is empty", 1);
        return;
    }
    //----------------------------------------------------------------------------
    std::unordered_map<std::string, std::vector<std::tuple<std::string, int>>> orders_by_seller;
    //----------------------------------------------------------------------------
    // Grouped the items by seller_id so each seller has a separate vector containing their items.
    for (const auto& [listing_key, listing_qty, seller_id] : cart.contents) {
        // Check if the seller_id is already a key in the orders_by_seller map
        if (orders_by_seller.count(seller_id) == 0) {
            // Seller_id not found in the map, so this is a new seller
            // Create a new entry in the map for the seller and initialize the vector
            orders_by_seller[seller_id] = std::vector<std::tuple<std::string, int>>();
        }
        // Add the item to the corresponding seller's vector in the map
        orders_by_seller[seller_id].emplace_back(listing_key, listing_qty);
        ////std::cout << seller_id << "(" << listing_key << ", x" << listing_qty << ")\n";
    }
    //----------------------------------------------------------------------------
    // Create a separate order for each seller and store them in a vector
    std::vector<Order> orders;
    std::unordered_map<std::string, nlohmann::json> listing_cache;
    //----------------------------------------------------------------------------
    for (const auto& [seller_id, items] : orders_by_seller) {
        ////std::cout << "Seller ID: " << seller_id << std::endl;
        // Create a separate order for each seller
        // Process each item in the seller's order_items vector
        std::vector<std::tuple<std::string, int, std::string>> order_items;
        double subtotal = 0.00, discount = 0.00, shipping_cost = 0.00, total = 0.00;
        std::string currency = "USD"; // default

        for (const auto& [listing_key, listing_qty] : items) {
            ////std::cout << "\tItem: " << listing_key << " Quantity: " << listing_qty << std::endl;
            // Look up listing_obj and perform calculations for each item in the seller's vector
            nlohmann::json listing_obj = get_listing_object(listing_key);
            if(!listing_obj.contains("metadata")) { std::cout << "No metadata found"; return; }
            std::string metadata = listing_obj["metadata"].get<std::string>();
            if(metadata != "listing") { neroshop::print("Invalid metadata\n", 1); return; }
            listing_cache.insert({listing_key, listing_obj});
            // Get product name
            std::string product_name = listing_obj["product"]["name"].get<std::string>();
            
            // Get currency that product is priced in
            currency = listing_obj["currency"].get<std::string>();
            
            // Get sales price
            double sales_price = listing_obj["price"].get<double>();
            
            // Calculate the subtotal (price of all items combined)
            subtotal += listing_qty * sales_price;
            // Deal with the discount later ...
        
            // Check again to see if item is still in stock
            int quantity = listing_obj["quantity"].get<int>();
            if(quantity < listing_qty) {
                neroshop::print("Order request failed (Reason: Quantity has surpassed the stock available for the following item: " + product_name + ")");
                return;
            }
            if(quantity <= 0) {
                neroshop::print("Order request failed (Reason: The following item is out of stock: " + listing_obj["product"]["name"].get<std::string>() + ")");
                return;
            }
            
            // Add each product to order_item of the same order_id
            order_items.emplace_back(listing_key, listing_qty, seller_id);
            // Only the seller can reduce stock quantity of each purchased item
        }
        //----------------------------------------------------------------------------
        // Calculate total
        // Convert price to Monero (XMR)
        double subtotal_monero = neroshop::Converter::convert_to_xmr(subtotal, currency);
        double discount_monero = neroshop::Converter::convert_to_xmr(discount, currency);
        double shipping_cost_monero = neroshop::Converter::convert_to_xmr(shipping_cost, currency);
        total = (subtotal - discount) + shipping_cost;
        double total_monero = neroshop::Converter::convert_to_xmr(total, currency);
        // Convert monero to piconero (for storing in DHT)
        double piconero = 0.000000000001;
        uint64_t subtotal_piconero = subtotal_monero / piconero;
        uint64_t discount_piconero = discount_monero / piconero;
        uint64_t shipping_cost_piconero = shipping_cost_monero / piconero;
        uint64_t total_piconero = total_monero / piconero;
        //----------------------------------------------------------------------------
        // Construct the order
        std::string order_id = neroshop::uuid::generate();
        std::string created_at = neroshop::timestamp::get_current_utc_timestamp();
        auto order_status = OrderStatus::New;
        std::string customer_id = cart.owner_id;
        auto payment_option = PaymentOption::Escrow;
        auto payment_coin = PaymentCoin::Monero;
        auto delivery_option = DeliveryOption::Delivery;
        std::string notes = shipping_address;
    
        /*std::string signature = wallet->sign_message(order_id, monero_message_signature_type::SIGN_WITH_SPEND_KEY);//std::cout << "signature: " << signature << "\n\n";
    
        #ifdef NEROSHOP_DEBUG
        auto result = wallet->verify_message(order_id, signature);
        std::cout << "\033[1mverified: " << (result == 1 ? "\033[32mpass" : "\033[91mfail") << "\033[0m\n";
        assert(result == true);
        #endif*/
    
        Order order = {
            order_id, created_at, order_status, customer_id, 
            subtotal_monero, discount_monero, shipping_cost_monero, total_monero, 
            payment_option, payment_coin, delivery_option, 
            notes, order_items
        };
        
        orders.push_back(order);
    } // for loop
    //----------------------------------------------------------------------------
    Client * client = Client::get_main_client();
    for (const auto& order : orders) {
        auto data = Serializer::serialize(order);
        std::string key = data.first;
        std::string value = data.second;
        std::cout << "key: " << data.first << "\nvalue: " << data.second << "\n";
    
        // Send put request to neighboring nodes (and your node too JIC)
        std::string response;
        client->put(key, value, response); // TODO: Error handling
        std::cout << "Received response: " << response << "\n";
        //----------------------------------------------------------------------------
        // Print order message
        neroshop::print("Thank you for using neroshop.");
        neroshop::io_write("You have ordered: ");
        for (const auto& [listing_key, listing_qty, seller_id] : cart.contents) {
            nlohmann::json listing_obj = listing_cache.at(listing_key);
            std::string product_name = listing_obj["product"]["name"].get<std::string>();
            std::cout << "\033[0;94m" + product_name << " (x" << listing_qty << ")\033[0m" << std::endl;
        }    
        /*// Display order details
        nlohmann::json json = nlohmann::json::parse(neroshop::load_json(), nullptr, false); // allow_exceptions set to false
        std::string your_currency = (json.is_discarded()) ? "USD" : json["preferred_currency"].get<std::string>();
        if(your_currency.empty() || !neroshop::Converter::is_supported_currency(your_currency)) your_currency = "USD"; // default //neroshop::Converter::from_xmr(neroshop::Converter::to_xmr(, currency), your_currency);
        neroshop::print("Sit tight as we notify the seller(s) about your order.");
        auto from = Converter::get_currency_enum(currency);
        auto to = Converter::get_currency_enum(your_currency);
        std::cout << "Subtotal: " << std::fixed << std::setprecision(12) << subtotal_monero << " xmr" << std::fixed << std::setprecision(2) << " (" << neroshop::Converter::get_currency_sign(your_currency) << (neroshop::Converter::get_price(from, to) * order.subtotal) << " " << neroshop::string::upper(your_currency) << ")" <<  std::endl; // (() ? : "")
        if(discount > 0) std::cout << "Discount: -" << std::fixed << std::setprecision(12) << discount_monero << " xmr" << std::fixed << std::setprecision(2) << " (-" << neroshop::Converter::get_currency_sign(your_currency) << (neroshop::Converter::get_price(from, to) * order.discount) << " " << neroshop::string::upper(your_currency) << ")" <<  std::endl;
        std::cout << "Shipping: " << std::fixed << std::setprecision(12) << shipping_cost_monero << " xmr" << std::fixed << std::setprecision(2) << " (" << neroshop::Converter::get_currency_sign(your_currency) << (neroshop::Converter::get_price(from, to) * order.shipping_cost) << " " << neroshop::string::upper(your_currency) << ")" <<  std::endl;
        std::cout << "Order total: " << std::fixed << std::setprecision(12) << total_monero << " xmr" << std::fixed << std::setprecision(2) << " (" << neroshop::Converter::get_currency_sign(your_currency) << (neroshop::Converter::get_price(from, to) * order.total) << " " << neroshop::string::upper(your_currency) << ")" <<  std::endl;*/
        //std::cout << "Estimated delivery date: " << delivery_date_est << std::endl;
    }
    //----------------------------------------------------------------------------
    // Empty cart after completing order
    const_cast<neroshop::Cart&>(cart).empty();    
}
////////////////////
void neroshop::Order::cancel_order()
{
    // cannot cancel order if it has been at least 12 hours or more
    // sellers can request that a buyer cancels an order
    // only a buyer can cancel an order
    set_status( OrderStatus::Cancelled );
}
////////////////////
void neroshop::Order::change_order()
{}
////////////////////
////////////////////
void neroshop::Order::set_id(const std::string& id) {
    this->id = id;
}

void neroshop::Order::set_date(const std::string& date) {
    this->date = date;
}

void neroshop::Order::set_status(OrderStatus status) { 
    this->status = status;
}

void neroshop::Order::set_status_by_string(const std::string& status) {
    if(status == "New") set_status(OrderStatus::New);
    if(status == "Pending") set_status(OrderStatus::Pending);
    if(status == "Processing") set_status(OrderStatus::Processing);
    if(status == "Shipped") set_status(OrderStatus::Shipped);
    if(status == "Ready For Pickup") set_status(OrderStatus::ReadyForPickup);
    if(status == "Delivered") set_status(OrderStatus::Delivered);
    if(status == "Cancelled") set_status(OrderStatus::Cancelled);
    if(status == "Failed") set_status(OrderStatus::Failed);
    if(status == "Returned") set_status(OrderStatus::Returned);
    if(status == "Disputed") set_status(OrderStatus::Disputed);
    if(status == "Declined") set_status(OrderStatus::Declined);
    //if(status == "") set_status(OrderStatus::);
}

void neroshop::Order::set_customer_id(const std::string& customer_id) {
    this->customer_id = customer_id;
}

void neroshop::Order::set_subtotal(double subtotal) {
    this->subtotal = subtotal;
}

void neroshop::Order::set_discount(double discount) {
    this->discount = discount;
}

void neroshop::Order::set_shipping_cost(double shipping_cost) {
    this->shipping_cost = shipping_cost;
}

void neroshop::Order::set_total(double total) {
    this->total = total;
}

void neroshop::Order::set_payment_option(PaymentOption payment_option) {
    this->payment_option = payment_option;
}

void neroshop::Order::set_payment_option_by_string(const std::string& payment_option) {
    if(payment_option == "Escrow") set_payment_option(PaymentOption::Escrow);
    if(payment_option == "Multisig") set_payment_option(PaymentOption::Multisig);
    if(payment_option == "Finalize") set_payment_option(PaymentOption::Finalize);
}

void neroshop::Order::set_payment_coin(PaymentCoin payment_coin) {
    this->payment_coin = payment_coin;
}

void neroshop::Order::set_payment_coin_by_string(const std::string& payment_coin) {
    if(payment_coin == "None") set_payment_coin(PaymentCoin::None);
    if(payment_coin == "Monero") set_payment_coin(PaymentCoin::Monero);
    //if(payment_coin == "") set_payment_coin(PaymentCoin::);
}

void neroshop::Order::set_delivery_option(DeliveryOption delivery_option) {
    this->delivery_option = delivery_option;
}

void neroshop::Order::set_delivery_option_by_string(const std::string& delivery_option) {
    if(delivery_option == "Delivery") set_delivery_option(DeliveryOption::Delivery);
    if(delivery_option == "Pickup") set_delivery_option(DeliveryOption::Pickup);
}

void neroshop::Order::set_notes(const std::string& notes) {
    this->notes = notes;
}

void neroshop::Order::set_items(const std::vector<std::tuple<std::string, int, std::string>>& items) {
    this->items = items;
}
////////////////////
////////////////////
std::string neroshop::Order::get_id() const {
    return id;
}

neroshop::OrderStatus neroshop::Order::get_status() const {
    return status;
}

std::string neroshop::Order::get_status_as_string() const {
    switch(status) {
        case OrderStatus::New: return "New"; break;
        case OrderStatus::Pending: return "Pending"; break;
        case OrderStatus::Processing: return "Processing"; break;
        case OrderStatus::Shipped: return "Shipped"; break;
        case OrderStatus::ReadyForPickup: return "Ready For Pickup"; break; //case OrderStatus::Ready: return "Ready For Pickup"; break;
        case OrderStatus::Delivered: return "Delivered"; break; //case OrderStatus::Done: return "Delivered"; break;
        case OrderStatus::Cancelled: return "Cancelled"; break;
        case OrderStatus::Failed: return "Failed"; break;
        case OrderStatus::Returned: return "Returned"; break;
        case OrderStatus::Disputed: return "Disputed"; break;
        case OrderStatus::Declined: return "Declined"; break;
        //case OrderStatus::: return ""; break;
        default: return "New";
    }
}

std::string neroshop::Order::get_date() const {
    return date;
}

std::string neroshop::Order::get_customer_id() const {
    return customer_id;
}

double neroshop::Order::get_subtotal() const {
    return subtotal;
}

double neroshop::Order::get_discount() const {
    return discount;
}

double neroshop::Order::get_shipping_cost() const {
    return shipping_cost;
}

double neroshop::Order::get_total() const {
    return total;
}

neroshop::PaymentOption neroshop::Order::get_payment_option() const {
    return payment_option;
}

std::string neroshop::Order::get_payment_option_as_string() const {
    switch(payment_option) {
        case PaymentOption::Escrow: return "Escrow"; break;
        case PaymentOption::Multisig: return "Multisig"; break;
        case PaymentOption::Finalize: return "Finalize"; break;
        default: return "Escrow";
    }
}

neroshop::PaymentCoin neroshop::Order::get_payment_coin() const {
    return payment_coin;
}

std::string neroshop::Order::get_payment_coin_as_string() const {
    switch(payment_coin) {
        case PaymentCoin::None: return "None"; break;
        case PaymentCoin::Monero: return "Monero"; break;
        //case PaymentCoin::: return ""; break;
        default: return "Monero";
    }
}

neroshop::DeliveryOption neroshop::Order::get_delivery_option() const {
    return delivery_option;
}

std::string neroshop::Order::get_delivery_option_as_string() const {
    switch(delivery_option) {
        case DeliveryOption::Delivery: return "Delivery"; break;
        case DeliveryOption::Pickup: return "Pickup"; break;
        default: return "Delivery";
    }
}

std::string neroshop::Order::get_notes() const {
    return notes;
}

std::vector<std::tuple<std::string, int, std::string>> neroshop::Order::get_items() const {
    return items;
}
////////////////////
////////////////////
bool neroshop::Order::is_cancelled() const {
    return (status == OrderStatus::Cancelled);
}
////////////////////
////////////////////
