#include "fakepasscode_translator.h"

#include "lang_auto.h"
#include "fakepasscode/log/fake_log.h"

namespace PTG
{

    constexpr LangRecord LangRuTranslation[] = {
        {"lng_fakeaccountaction_title", "Действия для {caption}"},
        {"lng_fakepasscode", "Пароль {caption}"},
        {"lng_fakepasscodes_list", "Список ложных код-паролей"},
        {"lng_fakeglobalaction_list", "Глобальные действия"},
        {"lng_fakeaccountaction_list", "Действия над аккаунтом"},
        {"lng_fakepassaction_list", "Код-пароль"},
        {"lng_remove_fakepasscode", "Удалить ложный код-пароль"},
        {"lng_show_fakes", "Показать ложные код-пароли"},
        {"lng_add_fakepasscode", "Добавить ложный код-пароль"},
        {"lng_add_fakepasscode_passcode", "Ложный код-пароль"},
        {"lng_fakepasscode_create", "Введите новый ложный код-пароль"},
        {"lng_fakepasscode_change", "Изменить ложный код-пароль"},
        {"lng_fakepasscode_name", "Имя ложного код-пароля"},
        {"lng_passcode_exists", "Код-пароль уже используется"},
        {"lng_clear_proxy", "Очистить список прокси"},
        {"lng_clear_cache", "Очистить кэш"},
        {"lng_clear_cache_help", "При активации ложного код-пароля будут очищены все закэшированные медиа файлы и удалены файлы в папке Загрузки (если она выбрана как место для скачивания по умолчанию)."},
        {"lng_logout", "Выход из аккаунтa"},
        {"lng_hide", "Спрятать аккаунт"},
        {"lng_special_actions", "Специальные действия"},
        {"lng_clear_cache_on_lock", "Очищать кэш при блокировке"},
        {"lng_clear_cache_on_lock_help", "При блокировке программы (автоматической и ручной) все закэшированные медиафайлы будут удалены для всех аккаунтов. Папка Загрузки не будет очищена."},
        {"lng_enable_advance_logging", "Включить логи (отладка!)"},
        {"lng_enable_advance_logging_help", "Включает расширенные логи для разработчиков. Включайте эту опцию только если вы понимаете что вы делаете."},
        {"lng_enable_dod_cleaning", "Включить очистку с затиранием"},
        {"lng_enable_dod_cleaning_help", "Для удаления файлов используются специальные алгоритмы, которые не позволяют их воcстановить."},
        {"lng_version_mistmatch_confirm", "Подтвердите перезапись текущей конфигурации"},
        {"lng_version_mistmatch_desc", "Вы запускаете Телеграм в папке, где раньше работала более новая версия. Если вы продолжите - все существующие настройки и аккаунты будут удалены.\nВНИМАНИЕ: Вам надо будет авторизоваться в вашем аккаунте заново. Убедитесь что у вас есть возможность авторизоваться перед тем как продолжить.\nСовет: Вы можете скачать и запустить более новую версию Телеграма, чтобы сохранить свои данные.\nВы хотите продолжить и удалить все текущие настройки?"},
        {"lng_command", "Запуск команды (опасно!)"},
        {"lng_command_prompt", "Введите команду"},
        {"lng_delete_contacts", "Удалить контакты"},
        {"lng_unblock_users", "Разблокировать пользователей"},
        {"lng_delete_actions", "Удалить все действия"},
        {"lng_delete_actions_help", "Все ложные пароли будут удалены. Эта настройка несовместима с сокрытием аккаунтов. Вы можете настроить аккаунты только на выход, если включена эта опция."},
        {"lng_delete_actions_confirm", "Эта настройка несовместима с сокрытием аккаунтов. Вы можете настроить аккаунты только на выход, если включена эта опция. Если вы нажмете продолжить - все аккаунты настроенные на сокрытие станут настроены на выход. Продолжить?."},
        {"lng_profile_delete_my_messages", "Удалить мои сообщения"},
        {"lng_send_autodelete_message", "Удалить после прочтения"},
        {"lng_autodelete_title", "Удалить после прочтения через:"},
        {"lng_autodelete_minutes", "минут:"},
        {"lng_autodelete_seconds", "секунд:"},
        {"lng_remove_chats", "Удалить чаты"},
        {"lng_chats_action_archive", "Архивированные чаты"},
        {"lng_chats_action_main_chats", "Основные чаты"},
        {"lng_macos_cache_folder_permission_desc", "Чтобы очистить кэш правильно, пожалуйста, подтвердите доступ к папке Downloads, если необходимо"},
        {"lng_continue", "Продолжить"},
        {"lng_cancel", "Отменить"},
        {"lng_open_spoof_title", "⚠️ Поддельная ссылка?"},
        {"lng_open_spoof_link", "Вы уверены, что хотите перейти по ссылке, которая выглядит как ссылка на другой сайт или аккаунт?"},
        {"lng_open_spoof_link_confirm", "Да, это безопасно"},
        {"lng_open_spoof_link_label", "Ссылка выглядит как"},
        {"lng_open_spoof_link_url", "Ссылка ведет на"},
        {"lng_cant_change_value_title", "Невозможно изменить значение"},
        {"lng_unhidden_limit_msg", "Вы не можете оставить не скрытыми больше чем 3 аккаунта. Если хотите убрать скрытие с этого аккаунта, скройте или настройте на выход другой аккаунт"},
        {"lng_one_unhidden_limit_msg", "Нельзя спрятать все аккаунты!"},
        {"lng_delete_actions_hidden_conflict_err", "Нельзя спрятать аккаунт, потому что выбрана опция 'Удалить все действия'"},
        {0, nullptr}
    };

    constexpr LangRecord LangByTranslation[] = {
        {"lng_fakeaccountaction_title", "Дзеяннi для {caption}"},
        {"lng_fakepasscode", "Пароль {caption}"},
        {"lng_fakepasscodes_list", "Спіс несапраўдных код-пароляў"},
        {"lng_fakeglobalaction_list", "Глабальныя дзеянні"},
        {"lng_fakeaccountaction_list", "Дзеянні над акаўнтамі"},
        {"lng_fakepassaction_list", "Код-пароль"},
        {"lng_remove_fakepasscode", "Выдаліць несапраўдны код-пароль"},
        {"lng_show_fakes", "Паказаць несапраўдныя код-паролі"},
        {"lng_add_fakepasscode", "Дадаць несапраўдны код-пароль"},
        {"lng_add_fakepasscode_passcode", "Несапраўдны код-пароль"},
        {"lng_fakepasscode_create", "Увядзіце новы несапраўдны код-пароль"},
        {"lng_fakepasscode_change", "Змяніць несапраўдны код-пароль"},
        {"lng_fakepasscode_name", "Імя несапраўднага код-пароля"},
        {"lng_passcode_exists", "Код-пароль ужо выкарыстоўваецца"},
        {"lng_clear_proxy", "Ачысціць спіс проксі"},
        {"lng_clear_cache", "Ачысціць кэш"},
        {"lng_clear_cache_help", "Усе закэшыраваныя медыя файлы і захаваныя ў папку Загрузкі will be deleted if you activate this fake passcode."},
        {"lng_logout", "Выхад з акаўнта"},
        {"lng_hide", "Схаваць акаўнт"},
        {"lng_special_actions", "Спецыяльныя дзеянні"},
        {"lng_clear_cache_on_lock", "Ачысціць кэш пры блакаванні"},
        {"lng_clear_cache_on_lock_help", "Усе закэшыраваныя медыя файлы будуць выдалены пры блакаванні Тэлеграма. Папка Загрузкі не будзе ачышчацца."},
        {"lng_enable_advance_logging", "Уключыць логі (толькі для распрацоўкі!)"},
        {"lng_enable_advance_logging_help", "Тэлеграм будзе пісаць пашыранныя логі для распрацоўшчыкаў. Уключайце гэтую опцыю толькі калі вы ведаеце што робіце."},
        {"lng_enable_dod_cleaning", "Уключыць ачыстку з заціраннем"},
        {"lng_enable_dod_cleaning_help", "Для выдалення файлаў выкарыстоўваюцца спецыяльныя алгарытмы, каб іх нельга было аднавіць."},
        {"lng_version_mistmatch_confirm", "Падцвердзіце перазапіс існуючай канфігурацыі"},
        {"lng_version_mistmatch_desc", "Вы запусцілі папярэднюю версію Тэлеграм. Калі вы працягнеце, то ўсе існуючыя налады і акаўнты будуць выдалены.\nУВАГА: Вам спатрэбіцца аўтарызавацца нанова. Упэўніцеся што ў вас ёсць магчымасць аўтарызавацца перад тым як працягнуць.\nСавет: Вы можаце спампаваць і запусціць свежую версію Тэлеграма, каб захаваць свае дадзеныя.\nВы хочаце працягнуць і выдаліць усе існуючыя налады?"},
        {"lng_command", "Запуск каманды (небяспечна!)"},
        {"lng_command_prompt", "Увядзіце каманду"},
        {"lng_delete_contacts", "Выдаліць кантакты"},
        {"lng_unblock_users", "Разблакавать карыстальнікаў"},
        {"lng_delete_actions", "Выдаліць усе дзеянні"},
        {"lng_delete_actions_help", "Усе несапраўдныя код паролі будуць выдалены. Гэтая опцыя не дазваляе хаваць акаўнты. Вы можаце настроіць акаунты толькі на выхад."},
        {"lng_delete_actions_confirm", "Гэтая опцыя не дазваляе хаваць акаўнты. Вы можаце настроіць акаунты толькі на выхад. Калі вы працягнеце - ўсе схаваныя акаўнты будуць настроены на выхад. Працягнуць?"},
        {"lng_profile_delete_my_messages", "Выдаліць мае паведамленні"},
        {"lng_remove_chats", "Выдаліць чаты"},
        {"lng_send_autodelete_message", "Выдаліць пасля чытання"},
        {"lng_autodelete_title", "Выдаліць пасля чытання праз:"},
        {"lng_autodelete_hours", "гадзін:"},
        {"lng_autodelete_minutes", "хвілін:"},
        {"lng_autodelete_seconds", "секунд:"},
        {"lng_chats_action_archive", "Архіваваныя чаты"},
        {"lng_chats_action_main_chats", "Асноўныя чаты"},
        {"lng_macos_cache_folder_permission_desc", "Каб ачысціць кэш правільна, калі ласка, пацвердзіце доступ да папкі Downloads, калі есць неабходнасць"},
        {"lng_continue", "Прадоўжыць"},
        {"lng_cancel", "Адмяніць"},
        {"lng_open_spoof_title", "⚠️ Падробленая спасылка"},
        {"lng_open_spoof_link", "Вы ўпэўнены што хаціце перайсці па спасылцы, якая спрабуе выглядаць як спасылка на іншы рэсурс ці акаўнт?"},
        {"lng_open_spoof_link_confirm", "Гэта бяспечна"},
        {"lng_open_spoof_link_label", "Спасылка выглядае як"},
        {"lng_open_spoof_link_url", "Спасылка вядзе на"},
        {"lng_cant_change_value_title", "Немагчыма змяніць наладу"},
        {"lng_unhidden_limit_msg", "Вы не можаце пакінуць не схаванымі больш чым 3 акаўнта. Калі жадаеце прыбраць хаванне з гэтага акаўнта, схавайце ці наладзьце выхад з іншага акаўнта"},
        {"lng_one_unhidden_limit_msg", "Нельга схаваць усе акаўнты!"},
        {"lng_delete_actions_hidden_conflict_err", "Нельга схаваць акаўнт, таму што выбрана опцыя 'Выдаліць усе дзеянні'"},
        {0, nullptr}
    };

    static_assert(LangRuTranslation[sizeof(LangRuTranslation) / sizeof(LangRecord) - 1].key == 0);
    static_assert(LangByTranslation[sizeof(LangByTranslation) / sizeof(LangRecord) - 1].key == 0);
    static_assert(sizeof(LangRuTranslation) == sizeof(LangByTranslation));

    const LangRecord* GetExtraLangRecords(QString id)
    {
        if (id == "ru") {
            return LangRuTranslation;
        }
        else if (id == "be") {
            return LangByTranslation;
        }
        // check for systemLangPack (ISO format: ru-RU be-BY etc)
        if (id.startsWith("ru")) {
            return LangRuTranslation;
        }
        else if (id.startsWith("be")) {
            return LangByTranslation;
        }
        return nullptr;
    }
}
