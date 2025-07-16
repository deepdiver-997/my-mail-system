#include "mail_system/back/mailServer/fsm/pop3s/pop3s_fsm.h"
#include <iostream>
#include <sstream>
#include <boost/algorithm/string.hpp>

namespace mail_system {

Pop3sFsm::Pop3sFsm(std::shared_ptr<DBPool> db_pool)
    : m_state(Pop3sState::AUTHORIZATION),
      m_userId(-1),
      m_dbPool(db_pool) {
}

Pop3sFsm::~Pop3sFsm() = default;

std::string Pop3sFsm::process_command(const std::string& command, const std::string& args) {
    try {
        // 处理不同的POP3命令
        if (command == "USER") {
            return handle_user(args);
        }
        else if (command == "PASS") {
            return handle_pass(args);
        }
        else if (command == "STAT") {
            return handle_stat();
        }
        else if (command == "LIST") {
            return handle_list(args);
        }
        else if (command == "RETR") {
            return handle_retr(args);
        }
        else if (command == "DELE") {
            return handle_dele(args);
        }
        else if (command == "NOOP") {
            return handle_noop();
        }
        else if (command == "RSET") {
            return handle_rset();
        }
        else if (command == "QUIT") {
            return handle_quit();
        }
        else if (command == "TOP") {
            return handle_top(args);
        }
        else if (command == "UIDL") {
            return handle_uidl(args);
        }
        else {
            return handle_unknown(command);
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Error processing POP3 command: " << e.what() << std::endl;
        return "-ERR Internal server error";
    }
}

Pop3sState Pop3sFsm::get_state() const {
    return m_state;
}

void Pop3sFsm::reset() {
    m_state = Pop3sState::AUTHORIZATION;
    m_username.clear();
    m_userId = -1;
    m_mails.clear();
    m_mailMap.clear();
}

bool Pop3sFsm::authenticate_user(const std::string& username, const std::string& password) {
    try {
        auto db_conn = m_dbPool->get_connection();
        if (!db_conn) {
            return false;
        }

        // 查询用户
        std::string sql = "SELECT id FROM users WHERE username = ? AND password = ?";
        auto result = db_conn->query(sql, username, password);
        
        m_dbPool->release_connection(db_conn);

        if (!result.empty()) {
            m_userId = std::stoi(result[0]["id"]);
            return true;
        }

        return false;
    }
    catch (const std::exception& e) {
        std::cerr << "Error authenticating user: " << e.what() << std::endl;
        return false;
    }
}

bool Pop3sFsm::load_user_mails() {
    try {
        auto db_conn = m_dbPool->get_connection();
        if (!db_conn) {
            return false;
        }

        // 查询用户的邮件
        std::string sql = "SELECT id, sender, recipients, subject, content, created_at, "
                         "LENGTH(content) as size FROM emails WHERE recipients LIKE ?";
        auto result = db_conn->query(sql, "%" + m_username + "%");
        
        m_dbPool->release_connection(db_conn);

        m_mails.clear();
        m_mailMap.clear();
        int msg_number = 1;

        for (const auto& row : result) {
            Pop3MailInfo mail;
            mail.id = std::stoi(row.at("id"));
            mail.from = row.at("sender");
            mail.to = row.at("recipients");
            mail.subject = row.at("subject");
            mail.content = row.at("content");
            mail.date = row.at("created_at");
            mail.size = std::stoi(row.at("size"));
            mail.deleted = false;

            m_mails.push_back(mail);
            m_mailMap[msg_number++] = m_mails.size() - 1;
        }

        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Error loading user mails: " << e.what() << std::endl;
        return false;
    }
}

bool Pop3sFsm::update_mail_status() {
    try {
        auto db_conn = m_dbPool->get_connection();
        if (!db_conn) {
            return false;
        }

        // 删除标记为删除的邮件
        for (const auto& mail : m_mails) {
            if (mail.deleted) {
                std::string sql = "DELETE FROM emails WHERE id = ?";
                db_conn->execute(sql, mail.id);
            }
        }

        m_dbPool->release_connection(db_conn);
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Error updating mail status: " << e.what() << std::endl;
        return false;
    }
}

std::string Pop3sFsm::handle_user(const std::string& args) {
    if (m_state != Pop3sState::AUTHORIZATION) {
        return "-ERR Command not valid in this state";
    }

    if (args.empty()) {
        return "-ERR Missing username";
    }

    m_username = args;
    return "+OK User accepted";
}

std::string Pop3sFsm::handle_pass(const std::string& args) {
    if (m_state != Pop3sState::AUTHORIZATION) {
        return "-ERR Command not valid in this state";
    }

    if (m_username.empty()) {
        return "-ERR Need username first";
    }

    if (args.empty()) {
        return "-ERR Missing password";
    }

    if (authenticate_user(m_username, args)) {
        m_state = Pop3sState::TRANSACTION;
        if (load_user_mails()) {
            return "+OK Logged in";
        }
        else {
            return "-ERR Error loading mailbox";
        }
    }

    return "-ERR Invalid username or password";
}

std::string Pop3sFsm::handle_stat() {
    if (m_state != Pop3sState::TRANSACTION) {
        return "-ERR Command not valid in this state";
    }

    size_t count = 0;
    size_t total_size = 0;

    for (const auto& mail : m_mails) {
        if (!mail.deleted) {
            count++;
            total_size += mail.size;
        }
    }

    return "+OK " + std::to_string(count) + " " + std::to_string(total_size);
}

std::string Pop3sFsm::handle_list(const std::string& args) {
    if (m_state != Pop3sState::TRANSACTION) {
        return "-ERR Command not valid in this state";
    }

    if (!args.empty()) {
        // 列出特定邮件
        try {
            int msg_number = std::stoi(args);
            auto it = m_mailMap.find(msg_number);
            if (it != m_mailMap.end() && !m_mails[it->second].deleted) {
                return "+OK " + std::to_string(msg_number) + " " + 
                       std::to_string(m_mails[it->second].size);
            }
            return "-ERR No such message";
        }
        catch (...) {
            return "-ERR Invalid message number";
        }
    }

    // 列出所有邮件
    std::stringstream ss;
    ss << "+OK Mailbox scan listing follows\r\n";
    
    for (const auto& pair : m_mailMap) {
        if (!m_mails[pair.second].deleted) {
            ss << pair.first << " " << m_mails[pair.second].size << "\r\n";
        }
    }
    
    ss << ".\r\n";
    return ss.str();
}

std::string Pop3sFsm::handle_retr(const std::string& args) {
    if (m_state != Pop3sState::TRANSACTION) {
        return "-ERR Command not valid in this state";
    }

    try {
        int msg_number = std::stoi(args);
        auto it = m_mailMap.find(msg_number);
        if (it != m_mailMap.end() && !m_mails[it->second].deleted) {
            const auto& mail = m_mails[it->second];
            std::stringstream ss;
            ss << "+OK " << mail.size << " octets\r\n"
               << "From: " << mail.from << "\r\n"
               << "To: " << mail.to << "\r\n"
               << "Subject: " << mail.subject << "\r\n"
               << "Date: " << mail.date << "\r\n"
               << "\r\n"
               << mail.content << "\r\n"
               << ".\r\n";
            return ss.str();
        }
        return "-ERR No such message";
    }
    catch (...) {
        return "-ERR Invalid message number";
    }
}

std::string Pop3sFsm::handle_dele(const std::string& args) {
    if (m_state != Pop3sState::TRANSACTION) {
        return "-ERR Command not valid in this state";
    }

    try {
        int msg_number = std::stoi(args);
        auto it = m_mailMap.find(msg_number);
        if (it != m_mailMap.end() && !m_mails[it->second].deleted) {
            m_mails[it->second].deleted = true;
            return "+OK Message deleted";
        }
        return "-ERR No such message";
    }
    catch (...) {
        return "-ERR Invalid message number";
    }
}

std::string Pop3sFsm::handle_noop() {
    if (m_state != Pop3sState::TRANSACTION) {
        return "-ERR Command not valid in this state";
    }

    return "+OK";
}

std::string Pop3sFsm::handle_rset() {
    if (m_state != Pop3sState::TRANSACTION) {
        return "-ERR Command not valid in this state";
    }

    // 取消所有删除标记
    for (auto& mail : m_mails) {
        mail.deleted = false;
    }

    return "+OK";
}

std::string Pop3sFsm::handle_quit() {
    std::string response;
    
    if (m_state == Pop3sState::TRANSACTION) {
        m_state = Pop3sState::UPDATE;
        if (update_mail_status()) {
            response = "+OK POP3 server signing off";
        }
        else {
            response = "-ERR Error updating mailbox";
        }
    }
    else {
        response = "+OK POP3 server signing off";
    }

    reset();
    return response;
}

std::string Pop3sFsm::handle_top(const std::string& args) {
    if (m_state != Pop3sState::TRANSACTION) {
        return "-ERR Command not valid in this state";
    }

    std::istringstream iss(args);
    std::string msg_number_str;
    std::string lines_str;
    iss >> msg_number_str >> lines_str;

    try {
        int msg_number = std::stoi(msg_number_str);
        int lines = std::stoi(lines_str);

        auto it = m_mailMap.find(msg_number);
        if (it != m_mailMap.end() && !m_mails[it->second].deleted) {
            const auto& mail = m_mails[it->second];
            std::stringstream ss;
            ss << "+OK Top of message follows\r\n"
               << "From: " << mail.from << "\r\n"
               << "To: " << mail.to << "\r\n"
               << "Subject: " << mail.subject << "\r\n"
               << "Date: " << mail.date << "\r\n"
               << "\r\n";

            // 只返回指定行数的邮件内容
            std::istringstream content_stream(mail.content);
            std::string line;
            int line_count = 0;
            while (line_count < lines && std::getline(content_stream, line)) {
                ss << line << "\r\n";
                line_count++;
            }

            ss << ".\r\n";
            return ss.str();
        }
        return "-ERR No such message";
    }
    catch (...) {
        return "-ERR Invalid arguments";
    }
}

std::string Pop3sFsm::handle_uidl(const std::string& args) {
    if (m_state != Pop3sState::TRANSACTION) {
        return "-ERR Command not valid in this state";
    }

    if (!args.empty()) {
        // 列出特定邮件的UIDL
        try {
            int msg_number = std::stoi(args);
            auto it = m_mailMap.find(msg_number);
            if (it != m_mailMap.end() && !m_mails[it->second].deleted) {
                return "+OK " + std::to_string(msg_number) + " " + 
                       std::to_string(m_mails[it->second].id);
            }
            return "-ERR No such message";
        }
        catch (...) {
            return "-ERR Invalid message number";
        }
    }

    // 列出所有邮件的UIDL
    std::stringstream ss;
    ss << "+OK UIDL listing follows\r\n";
    
    for (const auto& pair : m_mailMap) {
        if (!m_mails[pair.second].deleted) {
            ss << pair.first << " " << m_mails[pair.second].id << "\r\n";
        }
    }
    
    ss << ".\r\n";
    return ss.str();
}

std::string Pop3sFsm::handle_unknown(const std::string& command) {
    return "-ERR Unknown command";
}

// Pop3sFsmFactory实现

Pop3sFsmFactory::Pop3sFsmFactory(std::shared_ptr<DBPool> db_pool)
    : m_dbPool(db_pool) {
}

Pop3sFsmFactory::~Pop3sFsmFactory() = default;

std::unique_ptr<Pop3sFsm> Pop3sFsmFactory::create_fsm() {
    return std::make_unique<Pop3sFsm>(m_dbPool);
}

} // namespace mail_system