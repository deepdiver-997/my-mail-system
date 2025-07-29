#include <mail_system/back/mailServer/smtps_server.h>
#include <limits>
// #include <boost/mysql.hpp>
using namespace mail_system;
int main() {
    std::cin.tie(nullptr);
    try
    {
        ServerConfig config;
        // config.port = 465; // SMTPS默认端口
        // config.use_ssl = true;
        // config.certFile = "../../crt/server.crt";
        // config.keyFile = "../../crt/server.key";
        // config.require_auth = false;
        // config.use_database = true;
        // DBPoolConfig db_config;
        // db_config.achieve = "mysql";
        // db_config.host = "localhost";
        // db_config.user = "mail_test";
        // db_config.password = "abjskKA09qjf";
        // db_config.database = "mail_system";
        // config.db_pool_config = db_config;
        if(!config.loadFromFile("Config.json")) {
            std::cerr << "Failed to load config from file." << std::endl;
            return 1;
        }

        config.show();
        char check[16] = {0};
        std::cout << "Start server? (y/n)" << std::endl;
        std::cin >> check;
        if(check[0] != 'y' && check[0] != 'Y') {
            return 0;
        }

        SmtpsServer server(config);
        server.start();
        char cmd[256];
        while(true){
            memset(cmd,0,256);
            std::cout << "waiting for command:\n";
            std::cin.ignore(256, '\n');
            std::cin.getline(cmd,255);
            if(cmd[0] == 'q' || cmd[0] == 'Q') {
                server.stop();
                std::cout << "Server quit.\n";
                break;
            }
        //     if(memcpy(cmd, "pause", 5) == 0) {
        //         server.start();
        //         std::cout << "Server pause.\n";
        //     }
        //     if(memcpy(cmd, "start", 5) == 0) {
        //         server.start();
        //         std::cout << "Server start.\n";
        //     }
        }
    }
    catch (const boost::system::system_error& e) {
        std::cerr << "Boost system error: " << e.what() << std::endl;
        return 1;
    }
    catch (const std::runtime_error& e) {
        std::cerr << "Runtime error: " << e.what() << std::endl;
        return 1;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}