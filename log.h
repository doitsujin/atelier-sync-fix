#pragma once

#include <fstream>

#include "util.h"

namespace atfix {

class Log {

public:

  Log(const char* filename)
  : m_file(filename, std::ios::out | std::ios::trunc) {

  }

  template<typename... Args>
  void operator () (const Args&... args) {
    std::lock_guard lock(m_mutex);
    (m_file << ... << args) << std::endl;
  }

private:

  mutex         m_mutex;
  std::ofstream m_file;

};

}