#include <asio.hpp>
#include <asio/ssl.hpp>
#include <cctype>
#include <ctime>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using asio::ip::tcp;

struct TenderItem {
    std::string id;
    std::string title;
    std::string description;
};

struct QuerySpec {
    std::string label;
    std::string endpoint;
    std::string tType;
};

struct QueryResult {
    QuerySpec query;
    int total = 0;
    std::vector<TenderItem> items;
};

std::string unescape_json_string(const std::string& input) {
    std::string output;
    output.reserve(input.size());

    for (size_t i = 0; i < input.size(); ++i) {
        char ch = input[i];
        if (ch != '\\') {
            output.push_back(ch);
            continue;
        }

        if (i + 1 >= input.size()) {
            break;
        }

        char escaped = input[++i];
        switch (escaped) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u':
                if (i + 4 < input.size()) {
                    output.append("\\u");
                    output.append(input, i + 1, 4);
                    i += 4;
                }
                break;
            default:
                output.push_back(escaped);
                break;
        }
    }

    return output;
}

std::string extract_json_value(const std::string& text, const std::string& key) {
    size_t key_pos = text.find(key);
    if (key_pos == std::string::npos) {
        return {};
    }

    size_t colon = text.find(':', key_pos + key.size());
    if (colon == std::string::npos) {
        return {};
    }

    size_t value_start = text.find_first_not_of(" \t\r\n", colon + 1);
    if (value_start == std::string::npos) {
        return {};
    }

    if (text[value_start] == '"') {
        std::string raw;
        bool escape = false;
        for (size_t i = value_start + 1; i < text.size(); ++i) {
            char ch = text[i];
            if (escape) {
                raw.push_back('\\');
                raw.push_back(ch);
                escape = false;
                continue;
            }
            if (ch == '\\') {
                escape = true;
                continue;
            }
            if (ch == '"') {
                return unescape_json_string(raw);
            }
            raw.push_back(ch);
        }
        return unescape_json_string(raw);
    }

    size_t value_end = text.find_first_of(",}", value_start);
    if (value_end == std::string::npos) {
        value_end = text.size();
    }

    while (value_end > value_start && std::isspace(static_cast<unsigned char>(text[value_end - 1]))) {
        --value_end;
    }

    return text.substr(value_start, value_end - value_start);
}

std::vector<std::string> extract_item_objects(const std::string& body) {
    std::vector<std::string> items;
    size_t items_pos = body.find("\"items\":[");
    if (items_pos == std::string::npos) {
        return items;
    }

    size_t cursor = body.find('[', items_pos);
    if (cursor == std::string::npos) {
        return items;
    }

    ++cursor;
    bool in_string = false;
    bool escape = false;
    int depth = 0;
    size_t object_start = std::string::npos;

    for (; cursor < body.size(); ++cursor) {
        char ch = body[cursor];

        if (in_string) {
            if (escape) {
                escape = false;
            } else if (ch == '\\') {
                escape = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }

        if (ch == '"') {
            in_string = true;
            continue;
        }

        if (ch == '{') {
            if (depth == 0) {
                object_start = cursor;
            }
            ++depth;
            continue;
        }

        if (ch == '}') {
            if (depth > 0) {
                --depth;
                if (depth == 0 && object_start != std::string::npos) {
                    items.push_back(body.substr(object_start, cursor - object_start + 1));
                    object_start = std::string::npos;
                }
            }
            continue;
        }

        if (ch == ']' && depth == 0) {
            break;
        }
    }

    return items;
}

std::string format_date_utc(int offset_days) {
    time_t timestamp = time(NULL);
    struct tm datetime = *localtime(&timestamp);
    datetime.tm_mday += offset_days;
    std::mktime(&datetime);

    char date_buffer[20];
    strftime(date_buffer, sizeof(date_buffer), "%F", &datetime);
    return std::string(date_buffer) + "T00:00:05.000Z";
}

std::string build_query_path(const QuerySpec& query, int announce, int close, bool include_cDateF) {
    std::string path = query.endpoint + "?tType=" + query.tType + "&c=15";
    if (include_cDateF) {
        path += "&cDateF=" + format_date_utc(close);
    }
    path += "&pDateFrom=" + format_date_utc(-announce);
    path += "&isF=false&isA=false&sortBy=LastUpdateTime&sortDir=1";
    return path;
}

std::string build_tender_link(const QuerySpec& query, const TenderItem& item) {
    if (query.endpoint == "/api/nonESATA/filter/50") {
        return "https://esastar-publication-ext.sso.esa.int/nonEsaTenderActions/details/" + item.id;
    }

    return "https://esastar-publication-ext.sso.esa.int/ESATenderActions/details/" + item.id;
}

int default_announcement_days() {
    time_t timestamp = time(NULL);
    struct tm datetime = *localtime(&timestamp);

    if (datetime.tm_wday == 1) {
        return 3;
    }

    return 1;
}

class scraper {
    public:
        scraper(int announce, int close, bool include_cDateF = false)
            : announce(announce), close(close), include_cDateF(include_cDateF) {
            queries = {
                {"Tender Actions - Open Competition", "/api/tenderAction/filter/50", "5"},
                {"Tender Actions - Call for Proposals", "/api/tenderAction/filter/50", "7"},
                {"Non ESA Tender Actions - Open Competition", "/api/nonESATA/filter/50", "5"}
            };

            try {
                run();
            }
            catch (std::exception& e) {
                std::cerr << "Error: " << e.what() << "\n";
            }
        }

    private:
        std::string host = "esastar-publication-ext.sso.esa.int";
        int announce = 3;
        int close = 30;
        bool include_cDateF = false;
        std::vector<QuerySpec> queries;
        std::vector<QueryResult> results;

        std::string perform_request(const std::string& path) {
            asio::io_context io_context;

            asio::ssl::context ctx(asio::ssl::context::tls_client);
            ctx.set_default_verify_paths();
            ctx.set_options(asio::ssl::context::default_workarounds |
                            asio::ssl::context::no_sslv2 |
                            asio::ssl::context::no_sslv3 |
                            asio::ssl::context::single_dh_use);

            asio::ssl::stream<tcp::socket> ssl_socket(io_context, ctx);
            SSL_set_tlsext_host_name(ssl_socket.native_handle(), host.c_str());
            ssl_socket.set_verify_mode(asio::ssl::verify_peer);
            ssl_socket.set_verify_callback(asio::ssl::rfc2818_verification(host));

            tcp::resolver resolver(io_context);
            auto endpoints = resolver.resolve(host, "https");
            asio::connect(ssl_socket.lowest_layer(), endpoints);
            ssl_socket.handshake(asio::ssl::stream_base::client);

            std::string request =
                "GET " + path + " HTTP/1.1\r\n"
                "Host: " + host + "\r\n"
                "Connection: close\r\n\r\n";

            asio::write(ssl_socket, asio::buffer(request));

            asio::streambuf response;
            asio::read_until(ssl_socket, response, "\r\n");

            std::istream response_stream(&response);
            std::string http_version;
            unsigned int status_code = 0;
            std::string status_message;
            response_stream >> http_version >> status_code;
            std::getline(response_stream, status_message);

            std::cout << "Status: " << status_code << status_message << "\n";
            if (status_code != 200) {
                throw std::runtime_error("Unexpected HTTP status");
            }

            asio::read_until(ssl_socket, response, "\r\n\r\n");

            std::string header_line;
            while (std::getline(response_stream, header_line) && header_line != "\r") {
            }

            asio::error_code ec;
            asio::read(ssl_socket, response, asio::transfer_all(), ec);
            if (ec == asio::ssl::error::stream_truncated || ec == asio::error::eof) {
                ec.clear();
            }
            if (ec) {
                throw asio::system_error(ec);
            }

            return std::string(asio::buffers_begin(response.data()), asio::buffers_end(response.data()));
        }

        QueryResult parse_response(const QuerySpec& query, const std::string& body) {
            QueryResult result;
            result.query = query;

            size_t total_pos = body.find("\"total\":");
            if (total_pos != std::string::npos) {
                size_t total_start = total_pos + 8;
                size_t total_end = body.find_first_of(",}", total_start);
                if (total_end == std::string::npos) {
                    total_end = body.size();
                }
                result.total = std::stoi(body.substr(total_start, total_end - total_start));
            }

            for (const std::string& item_object : extract_item_objects(body)) {
                TenderItem item;
                item.id = extract_json_value(item_object, "\"id\"");
                item.title = extract_json_value(item_object, "\"title\"");
                item.description = extract_json_value(item_object, "\"description\"");
                if (!item.id.empty()) {
                    result.items.push_back(item);
                }
            }

            return result;
        }

        void print_results() const {
            int grand_total = 0;
            std::cout << "Queries executed: " << results.size() << "\n";

            for (const QueryResult& result : results) {
                grand_total += result.total;
                std::cout << "\n=== " << result.query.label << " ===\n";
                std::cout << "Total: " << result.total << "\n";

                for (const TenderItem& item : result.items) {
                    std::cout << "ID: " << item.id << "\n";
                    std::cout << "Title: " << item.title << "\n";
                    std::cout << "Description: " << item.description << "\n";
                    std::cout << "Link: " << build_tender_link(result.query, item) << "\n\n";
                }
            }

            std::cout << "\nGrand total: " << grand_total << "\n";
        }

        void run() {
            std::cout << "1st argument: number of days back to use in announcement date filter. Using: " << announce << "\n";
            std::cout << "2nd argument: number of days ahead to use in closing date filter. Using: " << close << "\n";
            std::cout << "3rd argument: closing date filter usage. Using: " << (include_cDateF ? "enabled" : "disabled") << "\n";

            for (const QuerySpec& query : queries) {
                std::string path = build_query_path(query, announce, close, include_cDateF);
                std::cout << "\nQuery: " << query.label << "\n";
                std::string body = perform_request(path);
                results.push_back(parse_response(query, body));
            }

            print_results();
        }
};

int main(int argc, char* argv[]) {
    int announcement = default_announcement_days();
    int closing = 30;
    bool include_cDateF = false;

    if (argc > 1) {
        int requested_announcement = atoi(argv[1]);
        if (requested_announcement > 0) {
            announcement = requested_announcement;
        }
    }
    if (argc > 2) {
        closing = atoi(argv[2]);
    }
    if (argc > 3) {
        include_cDateF = std::string(argv[3]) != "0";
    }

    scraper esascraper(announcement, closing, include_cDateF);
    return 0;
}