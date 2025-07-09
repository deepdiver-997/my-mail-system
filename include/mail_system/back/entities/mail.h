#pragma once

#include<iostream>
#include<string>
#include<ctime>

struct mail
{
    size_t id;                  // 主键
    std::string from;           // 发件人邮箱地址
    std::string to;             // 收件人邮箱地址
    std::string header;        // 邮件头
    std::string body;           // 邮件正文
    time_t send_time;          // 发送时间
    bool is_draft;             // 是否为草稿
    bool is_read;              // 是否已读
};

struct mailbox
{
    size_t id;                  // 主键
    size_t user_id;            // 所属用户ID
    std::string name;          // 邮箱名称
    bool is_system;            // 是否为系统默认邮箱（收件箱、发件箱、垃圾箱等）
    int box_type;              // 系统邮箱类型：1收件箱，2发件箱，3垃圾箱，4已删除
    time_t create_time;        // 创建时间
};

struct attachment
{
    size_t id;                  // 主键
    size_t mail_id;            // 所属邮件ID
    std::string filename;       // 文件名
    std::string filepath;       // 文件存储路径
    size_t file_size;          // 文件大小（字节）
    std::string mime_type;     // 文件MIME类型
    time_t upload_time;        // 上传时间
};