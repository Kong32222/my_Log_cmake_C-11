#include "CLog.h"
int main()
{
    Logger::getInstance().log(LogLevel::DEBUG, "{} 字节序处理和消息队列的控制 ",GET_LOG_PREFIX(false)); 
    return 0;
}