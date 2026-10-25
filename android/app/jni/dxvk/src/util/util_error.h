#pragma once

#include <string>
#include <cstdio>

namespace dxvk {
  
  /**
   * \brief DXVK error
   * 
   * A generic exception class that stores a
   * message. Exceptions should be logged.
   */
  class DxvkError {
    
  public:
    
    DxvkError() { }
    DxvkError(std::string&& message)
    : m_message(std::move(message)) {
      // Port: errors that escape to std::terminate would otherwise go unlogged.
      std::fprintf(stderr, "DxvkError: %s\n", m_message.c_str());
      std::fflush(stderr);
    }
    
    const std::string& message() const {
      return m_message;
    }
    
  private:
    
    std::string m_message;
    
  };
  
}