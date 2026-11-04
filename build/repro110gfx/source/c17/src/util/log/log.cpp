/*
 * This file is part of DXMT, Copyright (c) 2023 Feifan He
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 */

#include <iostream>
#include <utility>

#include "log.hpp"

#include "../util_env.hpp"
#include "util_string.hpp"

namespace dxmt {

Logger::Logger(const std::string &fileName)
    : m_minLevel(getMinLogLevel()), m_fileName(fileName) {}

Logger::~Logger() {}

/*
 * MacRunner guard: under the ARM64X EC entry the Logger Meyers-singleton's
 * std::ostream/stringstream machinery has an uninitialized vtable (static-init
 * order), so emitMsg() (which uses std::stringstream/operator<</std::endl)
 * crashes with a c0000005 vtable AV the first time DXMT logs during
 * D3D11CreateDevice (DXMT d3d11 rva 0x1fac78). DXMT's log output is dead under
 * wine anyway (every output channel produces zero bytes), so neutralize ALL log
 * entry points to a no-op. This covers every Logger::{trace,debug,info,warn,err,
 * log} call site (whole-class sweep), not just the one on the device-create path.
 * Remove once the ARM64X static-init root fix lands (graphics lane).
 */
#define MACRUNNER_DXMT_LOG_DISABLED 0

void Logger::trace(const std::string &message) {
#if !MACRUNNER_DXMT_LOG_DISABLED
  s_instance.emitMsg(LogLevel::Trace, message);
#else
  (void)message;
#endif
}

void Logger::debug(const std::string &message) {
#if !MACRUNNER_DXMT_LOG_DISABLED
  s_instance.emitMsg(LogLevel::Debug, message);
#else
  (void)message;
#endif
}

void Logger::info(const std::string &message) {
#if !MACRUNNER_DXMT_LOG_DISABLED
  s_instance.emitMsg(LogLevel::Info, message);
#else
  (void)message;
#endif
}

void Logger::warn(const std::string &message) {
#if !MACRUNNER_DXMT_LOG_DISABLED
  s_instance.emitMsg(LogLevel::Warn, message);
#else
  (void)message;
#endif
}

void Logger::err(const std::string &message) {
#if !MACRUNNER_DXMT_LOG_DISABLED
  s_instance.emitMsg(LogLevel::Error, message);
#else
  (void)message;
#endif
}

void Logger::log(LogLevel level, const std::string &message) {
#if !MACRUNNER_DXMT_LOG_DISABLED
  s_instance.emitMsg(level, message);
#else
  (void)level;
  (void)message;
#endif
}

void Logger::emitMsg(LogLevel level, const std::string &message) {
  if (level >= m_minLevel) {
    std::lock_guard<dxmt::mutex> lock(m_mutex);

    static std::array<const char *, 5> s_prefixes = {
        {"trace: ", "debug: ", "info:  ", "warn:  ", "err:   "}};

    const char *prefix = s_prefixes.at(static_cast<uint32_t>(level));

    if (!std::exchange(m_initialized, true)) {
#ifdef _WIN32
      HMODULE ntdll = GetModuleHandleA("ntdll.dll");

      if (ntdll)
        m_wineLogOutput = reinterpret_cast<PFN_wineLogOutput>(
            GetProcAddress(ntdll, "__wine_dbg_output"));
#endif
      auto path = getFileName(m_fileName);

      if (!path.empty())
        m_fileStream = std::ofstream(str::topath(path.c_str()).c_str());
    }

    std::stringstream stream(message);
    std::string line;

    while (std::getline(stream, line, '\n')) {
      std::stringstream outstream;
      outstream << prefix << line << std::endl;

      std::string adjusted = outstream.str();

      if (!adjusted.empty()) {
        if (m_wineLogOutput)
          m_wineLogOutput(adjusted.c_str());
        else
          std::cerr << adjusted;
      }

      if (m_fileStream)
        m_fileStream << adjusted;
    }
  }
}

std::string Logger::getFileName(const std::string &base) {
  std::string path = env::getEnvVar("DXMT_LOG_PATH");

  if (path == "none")
    return std::string();

  // Don't create a log file if we're writing to wine's console output
  if (path.empty() && m_wineLogOutput)
    return std::string();

  if (!path.empty() && *path.rbegin() != '/')
    path += '/';

  std::string exeName = env::getExeBaseName();
  path += exeName + "_" + base;
  return path;
}

LogLevel Logger::getMinLogLevel() {
  const std::array<std::pair<const char *, LogLevel>, 6> logLevels = {{
      {"trace", LogLevel::Trace},
      {"debug", LogLevel::Debug},
      {"info", LogLevel::Info},
      {"warn", LogLevel::Warn},
      {"error", LogLevel::Error},
      {"none", LogLevel::None},
  }};

  const std::string logLevelStr = env::getEnvVar("DXMT_LOG_LEVEL");

  for (const auto &pair : logLevels) {
    if (logLevelStr == pair.first)
      return pair.second;
  }

  return LogLevel::Info;
}

} // namespace dxmt
