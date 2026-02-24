#include "CLog.h"
void LogQueue::push(std::string log)
{
    // 加锁
    std::lock_guard<std::mutex> lock(mutex_);
    // 添加日志消息
    queue_.push(log);
    // 唤醒一个等待线程
    cond_val_.notify_one();
}

// msg: 传出参数;
bool LogQueue::pop(std::string &msg)
{
    // 一:函数整体 加锁
    std::unique_lock<std::mutex> lock(mutex_);

    // 消费逻辑 (阻塞等待)  C++11 条件变量的改进方式
    // lambda 表达式 返回true 函数继续往下走  [this]值传递的方式捕获 this指针 是没有问题的;
    // 二:  如果队列不为空 继续下面的操作 (一直枷锁状态)
    //      第1种. 推荐这样写  wait 解锁意思：线程被唤醒时， 如果队列为空返回值是false 解锁,不持有锁. 获取锁成功则返回 true

    // 如果shutdown_ == true 证明主线程要退出
    cond_val_.wait(lock, [this]
                   { return !queue_.empty() || this->is_shutdown_; });

    // 虚假唤醒（操作系统也会唤醒线程）
    // 第2种：老写法
    // while (queue_.empty())
    // {
    //     cond_val_.wait(lock);
    // }

    // 三: 消费逻辑
    if (this->is_shutdown_ && queue_.empty())
    {
        return false; // 队列已关闭且为空，返回失败
    }

    /*
    特殊情况:扩展接口（一次性清空）
    std::vector<std::string> LogQueue::drain()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> logs;
        while (!queue_.empty()) {
            logs.push_back(queue_.front());
            queue_.pop();
        }
        //TODO: 处理 logs;
    }
    */
    // 特殊情况  如果主线程要退出了, 但是子线程队列不为空 ,负责循环消费队列的成员
    if (is_shutdown_ && !queue_.empty())
    {
        while (is_shutdown_ && !queue_.empty())
        {
            msg = queue_.front(); // 感觉不用这个操作,直接[弹出]也行好像.
            queue_.pop();
        }
        std::cout << "没有来的及处理msg" << msg << std::endl;
        return false;
    }

    msg = queue_.front();
    queue_.pop();
    return true; // 成功消费
}

void LogQueue::shutdown()
{
    // 一:函数整体 加锁
    std::lock_guard<std::mutex> lock(mutex_);
    is_shutdown_ = true;
    cond_val_.notify_all();
}

Logger::Logger(const std::string &log_file, bool log_to_stdout)
    : log_file_(log_file, std::ios::out | std::ios::app), // 文件: 追加模式
      log_to_console_(log_to_stdout),
      exit_flag_(false)
{
    // 初始化线程
    // 首先判断是否打开文件
    if (!log_file_.is_open())
    {
        throw std::runtime_error("Failed to open log file."); // 抛出异常，不能兼容容错。
    }
    else
    {
        log_file_name_ = log_file; // 保存文件名
    }

    // 子线程 lambda 函数 作用: 一直往文件中打印东西,
    worker_thread_ = std::thread([this]()
                                 {
                                     std::string msg;
                                     while (this->exit_flag_ == false)
                                     {
                                         //  获取队列中的消息
                                         if (this->log_queue_.pop(msg))
                                         {
                                             //  输出到文件中
                                             this->log_file_ << msg;
                                             this->log_file_.flush(); // 刷新

                                             // 输出到终端
                                             if (log_to_console_)
                                             {
                                                 std::cout << msg;
                                             }

                                             // UDP 发送
                                             if (log_to_udp_ && udp_sock_ >= 0)
                                             {
                                                 sendto(udp_sock_, msg.c_str(),
                                                        msg.size(),
                                                        0,
                                                        (struct sockaddr *)&udp_addr_,
                                                        sizeof(udp_addr_));
                                             }

                                             // 更新大小
                                             current_file_size_ += msg.size();
                                             if (current_file_size_ >= max_file_size_)
                                             {
                                                 rotateLogFile(); // 文件太大，切换新文件
                                             }
                                         }
                                     }
                                     // std::cout<<"worker thread exit"<<std::endl;
                                 });
}

Logger::~Logger()
{
    exit_flag_ = true;
    log_queue_.shutdown();         // 关闭队列
    if (worker_thread_.joinable()) // 没有分离态,就可以 join
    {
        worker_thread_.join(); // 等待线程结束  如果没有detach（分离）
    }

    if (log_file_.is_open())
    {
        // 关闭文件
        log_file_.close();
    }

    closeUdp(); // 关闭UDP
}

void Logger::log_exit()
{
    exit_flag_ = true;
}

void Logger::closeUdp()
{
    if (udp_sock_ >= 0)
    {
        close(udp_sock_);
        udp_sock_ = -1;
        log_to_udp_ = false;
    }
}

std::string Logger::getCurrentTime()
{

    // // 获取当前时间
    auto now = std::chrono::system_clock::now();
    // // 将时间转为字符串
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);

    // #第一种方式
    //  return std::ctime(&now_time); // ctime() 函数返回一个字符串，该字符串描述了给定时间

    // #第二种方式
    static char buf[100] = {0};
    // 格式化
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now_time)))
    {
        return std::string(buf);
    }
    else
    {
        return {}; // 出错返回空字符串
    }
}

void Logger::rotateLogFile()
{
    if (log_file_.is_open())
    {
        log_file_.close();
    }
    std::ostringstream oss;
    oss << log_file_name_ << "_" << file_index_++ << ".log";
    log_file_.open(oss.str(), std::ios::out | std::ios::app);
    current_file_size_ = 0;
}

void Logger::logToUdp(const std::string &ip, uint16_t port)
{
    udp_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock_ < 0)
    {
        perror("UDP socket create failed");
        return;
    }

    udp_addr_.sin_family = AF_INET;
    udp_addr_.sin_port = htons(port);
    udp_addr_.sin_addr.s_addr = inet_addr(ip.c_str());

    log_to_udp_ = true;
}

std::string getLogPrefix(const char *file, const char *function, int line, bool need_time)
{
    if (need_time)
    {
        char timeStr[32];
        std::time_t now = std::time(nullptr);
        std::tm *localTime = std::localtime(&now);
        std::strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localTime);

        std::ostringstream oss;
        oss << "[" << timeStr << "] " << "[" << file << ":" << function << ":" << line << "]";
        return oss.str();
    }

    std::ostringstream oss;
    //oss << "[" << file << ":" << function << ":" << line << "]";
    oss << "[" << function << ":" << line << "]";

    return oss.str();
}
