#ifndef TELEGRAM_AUTODELETE_CALLBACK_H
#define TELEGRAM_AUTODELETE_CALLBACK_H

#include <gsl/pointers>
#include <qstring.h>
#include <base/basic_types.h>
#include <base/object_ptr.h>

namespace Api {
struct SendOptions;
}

namespace Ui {
class BoxContent;
}

namespace FakePasscode {

bool DisableAutoDeleteInContextMenu();

Fn<void()> DefaultAutoDeleteCallback(
        not_null<QWidget*> quard,
        Fn<void(object_ptr<Ui::BoxContent>)> show,
        Fn<void(Api::SendOptions)> send);

}

#endif //TELEGRAM_AUTODELETE_CALLBACK_H
