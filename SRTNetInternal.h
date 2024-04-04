//
// Created by Anders Cedronius on 2019-04-22.
//

#ifndef CPPSRTWRAPPER_SRTGLOBALHANDLER_H
#define CPPSRTWRAPPER_SRTGLOBALHANDLER_H

#include <iostream>
#include <sstream>
#include <sys/syslog.h>

// Global Logger -- Start
#define LOGG_NOTIFY LOG_NOTICE
#define LOGG_WARN LOG_WARNING
#define LOGG_ERROR LOG_ERR
#define LOGG_FATAL LOG_CRIT

#ifdef DEBUG
#define SRT_LOGGER(l,g,f) \
{ \
  if (g <= gLogLevel) { \
    std::ostringstream a; \
    if (SRTNet::gLogHandler == SRTNet::defaultLogHandler) { \
      if (g == LOG_DEBUG) {a << "Debug: ";} \
      else if (g == LOG_INFO) {a << "Info: ";} \
      else if (g == LOG_NOTICE) {a << "Notification: ";} \
      else if (g == LOG_WARNING) {a << "Warning: ";} \
      else if (g == LOG_ERR) {a << "Error: ";} \
      else if (g == LOG_CRIT) {a << "Critical: ";} \
      else if (g == LOG_ALERT) {a << "Alert: ";} \
      else if (g == LOG_EMERG) {a << "Emergency: ";} \
      if (l) {a << __FILE__ << " " << __LINE__ << " ";} \
    } \
    if (!mLogPrefix.empty()) { \
      a << mLogPrefix << ": "; \
    } \
    a << f; \
    SRTNet::gLogHandler(nullptr, g, __FILE__, __LINE__, nullptr, a.str().c_str()); \
  } \
}
#else
#define SRT_LOGGER(l,g,f)
#endif
// GLobal Logger -- End

#endif //CPPSRTWRAPPER_SRTGLOBALHANDLER_H
