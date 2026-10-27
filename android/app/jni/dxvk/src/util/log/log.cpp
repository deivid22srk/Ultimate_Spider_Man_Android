#include "log.h"

#include "../util_env.h"

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace dxvk {
  
  Logger::Logger(const std::string& file_name)
  : m_minLevel(getMinLogLevel()) {
    if (m_minLevel != LogLevel::None) {
      auto path = getFileName(file_name);

      if (!path.empty()) {
#ifdef _WIN32
        m_fileStream = std::ofstream(str::tows(path.c_str()).c_str());
#else
        m_fileStream = std::ofstream(path.c_str());
#endif
      }
    }
  }
  
  
  Logger::~Logger() { }
  
  
  void Logger::trace(const std::string& message) {
    s_instance.emitMsg(LogLevel::Trace, message);
  }
  
  
  void Logger::debug(const std::string& message) {
    s_instance.emitMsg(LogLevel::Debug, message);
  }
  
  
  void Logger::info(const std::string& message) {
    s_instance.emitMsg(LogLevel::Info, message);
  }
  
  
  void Logger::warn(const std::string& message) {
    s_instance.emitMsg(LogLevel::Warn, message);
  }
  
  
  void Logger::err(const std::string& message) {
    s_instance.emitMsg(LogLevel::Error, message);
  }
  
  
  void Logger::log(LogLevel level, const std::string& message) {
    s_instance.emitMsg(level, message);
  }
  
  
  void Logger::emitMsg(LogLevel level, const std::string& message) {
    if (level >= m_minLevel) {
      std::lock_guard<dxvk::mutex> lock(m_mutex);
      
      static std::array<const char*, 5> s_prefixes
        = {{ "trace: ", "debug: ", "info:  ", "warn:  ", "err:   " }};
      
      const char* prefix = s_prefixes.at(static_cast<uint32_t>(level));

#ifdef __ANDROID__
      /* Port: the port's stderr is redirected to <game>/port/log.txt, so DXVK's
       * own messages never show up in a logcat capture. Mirror them, otherwise
       * a phone bug report carries the crash but no reason for it. */
      static const std::array<int, 5> s_prios
        = {{ ANDROID_LOG_VERBOSE, ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR }};
      const int prio = s_prios.at(static_cast<uint32_t>(level));
#endif

      std::stringstream stream(message);
      std::string       line;

      while (std::getline(stream, line, '\n')) {
        std::cerr << prefix << line << std::endl;

#ifdef __ANDROID__
        __android_log_print(prio, "DXVK", "%s%s", prefix, line.c_str());
#endif

        if (m_fileStream)
          m_fileStream << prefix << line << std::endl;
      }
    }
  }
  
  
  LogLevel Logger::getMinLogLevel() {
    const std::array<std::pair<const char*, LogLevel>, 6> logLevels = {{
      { "trace", LogLevel::Trace },
      { "debug", LogLevel::Debug },
      { "info",  LogLevel::Info  },
      { "warn",  LogLevel::Warn  },
      { "error", LogLevel::Error },
      { "none",  LogLevel::None  },
    }};
    
    const std::string logLevelStr = env::getEnvVar("DXVK_LOG_LEVEL");
    
    for (const auto& pair : logLevels) {
      if (logLevelStr == pair.first)
        return pair.second;
    }
    
    return LogLevel::Info;
  }
  
  
  std::string Logger::getFileName(const std::string& base) {
    std::string path = env::getEnvVar("DXVK_LOG_PATH");
    
    if (path == "none")
      return "";

    if (!path.empty() && *path.rbegin() != '/')
      path += '/';

    std::string exeName = env::getExeBaseName();
    path += exeName + "_" + base;
    return path;
  }
  
}
