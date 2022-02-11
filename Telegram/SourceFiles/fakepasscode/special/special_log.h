#pragma once

#include "base/debug_log.h"

namespace base {

	void LogSpecialMain(const QString& message, const char *file);

}

#define SPECIAL_LOG(message) (::base::LogSpecialMain(QString message, SOURCE_FILE_BASENAME))
