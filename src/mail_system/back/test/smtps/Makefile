CXX = clang++
CXXFLAGS = -std=c++17 -I/opt/homebrew/include \
           -I../../../../../include \
		   -I/Users/zhuhongrui/Desktop/code/c++/OuterLib/json/single_include \
           -Wall -g

LDFLAGS = -L/opt/homebrew/lib \
          -lboost_system -lboost_thread -lssl -lcrypto -lmysqlclient -lpthread

# 源文件列表（只列出cpp文件）
SRCS = test.cpp \
	   ../../../../../src/mail_system/back/mailServer/server_base.cpp \
	   ../../../../../src/mail_system/back/mailServer/session/session_base.cpp \
	   ../../../../../src/mail_system/back/mailServer/smtps/smtps_server.cpp \
	   ../../../../../src/mail_system/back/mailServer/session/smtps_session.cpp \
	   ../../../../../src/mail_system/back/mailServer/fsm/smtps/smtps_fsm.cpp \
	   ../../../../../src/mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.cpp \
	   ../../../../../src/mail_system/back/db/mysql_pool.cpp \
	   ../../../../../src/mail_system/back/db/mysql_service.cpp \

# 自动生成的目标文件列表
OBJS = $(SRCS:.cpp=.o)

TARGET = test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean