#include "special_log.h"

#include "base/debug_log.h"
#include "storage/storage_domain.h"
#include "core/application.h"
#include "main/main_domain.h"

void base::LogSpecialMain(const QString& message, const char *file) {
	if (Core::App().domain().local().IsSpecialLoggingEnabled()) {
		LogWriteDebug(message, file, __LINE__);
	}
}
