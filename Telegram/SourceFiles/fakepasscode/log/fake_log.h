#pragma once

#include "base/debug_log.h"

namespace base {

void LogFakeMain(const QString& message, const char *file, const int line);

}

#define FAKE_LOG(message) (::base::LogFakeMain(QString message, SOURCE_FILE_BASENAME, __LINE__))

#define FAKE_TRACE() (::base::LogFakeMain(QString(__FUNCTION__), SOURCE_FILE_BASENAME, __LINE__))
