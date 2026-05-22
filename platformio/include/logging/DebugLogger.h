#pragma once

#include <Arduino.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

/*
  SmartFires structured debug logger.

  Example output:

    @SFDBG	v=1	node=1	src=sht31	lvl=I	seq=42	t=123456	msg=sampled temp_c=22.18 humidity_pct=31.90

  This is intentionally line-based and human-readable.
*/

#ifndef SMARTFIRES_DEBUG_LOG
#define SMARTFIRES_DEBUG_LOG 1
#endif

#ifndef SMARTFIRES_LOG_MSG_MAX
#define SMARTFIRES_LOG_MSG_MAX 160
#endif

#ifndef SMARTFIRES_LOG_SRC_MAX
#define SMARTFIRES_LOG_SRC_MAX 24
#endif

enum class LogLevel : uint8_t {
  Trace = 0,
  Debug = 1,
  Info  = 2,
  Warn  = 3,
  Error = 4,
  Off   = 5,
};

class DebugLogger {
public:
  DebugLogger(Print &out, uint8_t nodeId)
      : _out(out), _nodeId(nodeId), _minLevel(LogLevel::Trace), _seq(0) {}

  void setNodeId(uint8_t nodeId) {
    _nodeId = nodeId;
  }

  void setMinLevel(LogLevel level) {
    _minLevel = level;
  }

  LogLevel minLevel() const {
    return _minLevel;
  }

  bool enabled(LogLevel level) const {
#if SMARTFIRES_DEBUG_LOG
    return static_cast<uint8_t>(level) >= static_cast<uint8_t>(_minLevel) &&
           _minLevel != LogLevel::Off;
#else
    (void)level;
    return false;
#endif
  }

  void log(LogLevel level, const char *src, const char *msg) {
#if SMARTFIRES_DEBUG_LOG
    if (!enabled(level)) {
      return;
    }

    _out.print(F("@SFDBG"));

    _out.print('\t');
    _out.print(F("v=1"));

    _out.print('\t');
    _out.print(F("node="));
    _out.print(_nodeId);

    _out.print('\t');
    _out.print(F("src="));
    printEscaped(src, SMARTFIRES_LOG_SRC_MAX);

    _out.print('\t');
    _out.print(F("lvl="));
    _out.print(levelChar(level));

    _out.print('\t');
    _out.print(F("seq="));
    _out.print(_seq++);

    _out.print('\t');
    _out.print(F("t="));
    _out.print(millis());

    _out.print('\t');
    _out.print(F("msg="));
    printEscaped(msg, SMARTFIRES_LOG_MSG_MAX);

    _out.print('\n');
#else
    (void)level;
    (void)src;
    (void)msg;
#endif
  }

  void logf(LogLevel level, const char *src, const char *fmt, ...) {
#if SMARTFIRES_DEBUG_LOG
    if (!enabled(level)) {
      return;
    }

    char msg[SMARTFIRES_LOG_MSG_MAX];

    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    log(level, src, msg);
#else
    (void)level;
    (void)src;
    (void)fmt;
#endif
  }

private:
  Print &_out;
  uint8_t _nodeId;
  LogLevel _minLevel;
  uint32_t _seq;

  static char levelChar(LogLevel level) {
    switch (level) {
    case LogLevel::Trace:
      return 'T';
    case LogLevel::Debug:
      return 'D';
    case LogLevel::Info:
      return 'I';
    case LogLevel::Warn:
      return 'W';
    case LogLevel::Error:
      return 'E';
    case LogLevel::Off:
    default:
      return 'O';
    }
  }

  void printEscaped(const char *s, size_t maxChars) {
    if (s == nullptr) {
      return;
    }

    size_t n = 0;
    while (*s != '\0' && n < maxChars) {
      const char c = *s++;

      switch (c) {
      case '\\':
        _out.print(F("\\\\"));
        break;
      case '\t':
        _out.print(F("\\t"));
        break;
      case '\n':
        _out.print(F("\\n"));
        break;
      case '\r':
        _out.print(F("\\r"));
        break;
      default:
        _out.print(c);
        break;
      }

      ++n;
    }
  }
};

extern DebugLogger gLog;

#if SMARTFIRES_DEBUG_LOG
#define LOG_TRACE(src, fmt, ...) gLog.logf(LogLevel::Trace, src, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(src, fmt, ...) gLog.logf(LogLevel::Debug, src, fmt, ##__VA_ARGS__)
#define LOG_INFO(src, fmt, ...)  gLog.logf(LogLevel::Info,  src, fmt, ##__VA_ARGS__)
#define LOG_WARN(src, fmt, ...)  gLog.logf(LogLevel::Warn,  src, fmt, ##__VA_ARGS__)
#define LOG_ERROR(src, fmt, ...) gLog.logf(LogLevel::Error, src, fmt, ##__VA_ARGS__)
#else
#define LOG_TRACE(src, fmt, ...)
#define LOG_DEBUG(src, fmt, ...)
#define LOG_INFO(src, fmt, ...)
#define LOG_WARN(src, fmt, ...)
#define LOG_ERROR(src, fmt, ...)
#endif
