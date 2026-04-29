#pragma once

#include <QtCore/QString>

namespace Iv::Markdown {

[[nodiscard]] bool TryOpenLocalFile(const QString &path);

} // namespace Iv::Markdown
