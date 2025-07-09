#pragma once

#include<iostream>
#include<string>

struct usr
{
    size_t id;                  //  主键    和邮箱号一一对应
    std::string mailAddress;    //  必须唯一    包含邮箱后缀
    std::string password;       //  密码加密存储
    std::string name;           //  可以重复
    std::string telephone;
    time_t registerTime;
};