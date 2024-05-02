#ifndef TELEGRAM_FAKEPASSCODE_TRANSLATOR_H
#define TELEGRAM_FAKEPASSCODE_TRANSLATOR_H

#include <QString>
#include <QByteArray>

namespace PTG
{

    struct LangRecord
    {
        const char* key;
        const char* value;
    };

    const LangRecord* GetExtraLangRecords(QString id);

}

#endif //TELEGRAM_FAKEPASSCODE_TRANSLATOR_H
