#ifndef LOG_H
#define LOG_H

#include <memory>    // 智能指针
#include <thread>    // 异步打印
#include <atomic>    // 原子变量
#include <stdexcept> // 异常处理
#include <iostream>
#include <functional>
#include <algorithm>

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>  //udp close
#include <queue>              //队列
#include <mutex>              //互斥锁
#include <condition_variable> // 条件变量

#include <fstream> //写入文件中

#include <sstream> // 字符串流

#include <chrono> //原子变量


#include <arpa/inet.h>  // sockaddr_in, inet_addr
#include <sys/socket.h> // socket, sendto

// 工具函数：生成日志前缀
std::string getLogPrefix(const char *file, const char *function, int line, bool need_time = false);

// 宏定义：自动带上 文件名、函数名、行号
#define GET_LOG_PREFIX(need_time) getLogPrefix(__FILE__, __FUNCTION__, __LINE__, need_time)
/**
 * @brief 将输入值转换为字符串表示
 *
 * 该函数使用ostringstream将任意类型的值转换为字符串形式，
 * 通过完美转发保持参数的值类别特性
 *
 * @tparam T 输入值的类型（模板参数）
 * @param value 需要转换为字符串的值，通过右值引用和完美转发处理
 * @return std::string 返回转换后的字符串表示
 * 窍门：当这个模板是万能引用的时候，传入的参数默认转发到模板参数T中
 */
template <typename T>
std::string to_string_helper(T &&value)
{
    // 转发输入参数
    std::ostringstream oss;        //  创建字符串输出流
    oss << std::forward<T>(value); // 完美转发: 保存之前的输入参数类型
    return oss.str();
}

class LogQueue
{
public:
    void push(std::string log); //  添加日志消息
    bool pop(std::string &msg); // 函数调用返回值表示是否成功
    void shutdown();            // 关闭队列

private:
    std::queue<std::string> queue_;
    std::mutex mutex_;
    std::condition_variable cond_val_;
    bool is_shutdown_ = false;
};

enum LogLevel
{
    DEBUG,
    INFO,
    WARNING,
    ERROR
};

/**
 * @brief 日志记录器类
 *
 * 该类实现了一个单例模式的日志记录器，提供了日志记录和刷新功能。
 * 使用Log Queue来管理日志消息的队列，并在需要时进行异步处理。
 */
class Logger
{
public:
    /*****公共接口是 */
     // 获取单例
    static Logger& getInstance(const std::string &log_file = "log.txt",bool to_console = false) 
    {
        static Logger instance(log_file, to_console); // C++11 之后局部静态变量线程安全
        return instance;
    }

    // 禁止拷贝和赋值
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    ~Logger();

    template <typename... Args>
    void log(LogLevel logLevel, const std::string &format, Args &&...args)
    {
        process(logLevel, format, std::forward<Args>(args)...);
    } 
    void log_exit();
    void closeUdp();
    // 初始化 UDP
    void logToUdp(const std::string &ip = "127.0.0.1", uint16_t port = 9990);

private: // 私有函数
   
    Logger(const std::string &log_file, bool to_console);


    template <typename... Args>
    void process(LogLevel level, const std::string &format, Args &&...args);

    /*函数的功能是: log("我是{},今年{}岁, "孔祥森","25"}")     
    {} 是 自定义的格式,不是语法 */
    template <typename... Args>
    std::string fromatMessage(const std::string &format, Args... args);

    // 获取当前的时间
    std::string getCurrentTime();
    void rotateLogFile();

   

private: // 私有成员变量
    LogQueue log_queue_;
    std::thread worker_thread_;   // 异步打印线程
    std::ofstream log_file_;      // 日志文件
    std::atomic<bool> exit_flag_; // 退出标志

    bool log_to_console_ = false; // 是否输出到终端

    /*当文件大小超过 max_file_size_ 时调用 rotateLogFile()，创建新的日志文件。
    文件名带序号，便于归档 。   */
    std::string log_file_name_;                 // 日志文件基础名
    size_t max_file_size_ = 1 * 1024 * 1024;   // 最大文件大小，比如1MB
    size_t current_file_size_ = 0;              // 当前文件大小，用于监控
    int file_index_ = 0;                        // 文件序号


    /*转发UDP */
    bool log_to_udp_ = false;       // 是否发送 UDP
    int udp_sock_ = -1;             // UDP 套接字
    struct sockaddr_in udp_addr_;   // UDP 目标地址

};


template <typename... Args>
inline void Logger::process(LogLevel level, const std::string &format, Args &&...args)
{
    std::string logLevel_str;
    switch (level)
    {
    case LogLevel::DEBUG:
        logLevel_str = "[DEBUG] ";
        break;
    case LogLevel::INFO:
        logLevel_str = "[INFO] ";
        break;
    case LogLevel::WARNING:
        logLevel_str = "[WARNING] ";
        break;
    case LogLevel::ERROR:
        logLevel_str = "[ERROR] ";
        break;
    default:
        logLevel_str = "[UNKNOWN] ";
        break;
    }
    logLevel_str += " \t";
    log_queue_.push(logLevel_str + fromatMessage(format, std::forward<Args>(args)...));
}

// format = "this is {} , that is {}"
template <typename... Args>
std::string Logger::fromatMessage(const std::string &format, Args... args)
{
    /***首先放在字符串 容器 中(TODO: 难点)***/
    std::vector<std::string> args_strs = {to_string_helper(std::forward<Args>(args))...}; // 把所有参数都转换成字符串

    /***拼接字符串***/
    std::ostringstream oss;
    size_t arg_index = 0;
    size_t pos = 0; // 起始的位置

    // 左花括号的位置  placeholder:占位符
    size_t placeholder = format.find("{}", pos); //

    // 有花括号: 只要不等于 字符串末尾
    while (placeholder != std::string::npos)
    {
        // 首先是 左花括号的位置
        oss << format.substr(pos, placeholder - pos);
        if (arg_index < args_strs.size())
        {
            //++  下一个参数
            oss << args_strs[arg_index++]; // 怕调用的时候 没有字符串, 全是{} {}  {}  log("{} {} {}" );
        }
        else
        {
            oss << "{}"; // 输出一个{}
        }

        pos = placeholder + 2; // 跳过{}
        placeholder = format.find("{}", pos);
    }

    oss << format.substr(pos);
    // 特殊情况:处理多余参数
    if (arg_index < args_strs.size())
    {
        while (arg_index < args_strs.size()) // 没有花括号
        {
            oss << " " << args_strs[arg_index++]; // 加一个空格 " "
        }
    }
    oss << "\n"; // 然后换行  ,这样输出的日志格式自带换行符
    
    return "["+ getCurrentTime()+ "]\t" + oss.str();
}



#endif // LOG_H