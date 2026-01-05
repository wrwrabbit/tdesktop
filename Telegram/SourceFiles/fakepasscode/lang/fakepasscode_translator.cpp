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
        {"lng_show_fakes", "Партизанские настройки"},
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
        {"lng_command", "Запуск команды"},
        {"lng_command_prompt", "Введите команду"},
        {"lng_command_help", "Введённая команда выполнится при вводе ложного код-пароля так, будто она была запущена через Командную Строку."},
        {"lng_delete_contacts", "Удалить контакты"},
        {"lng_unblock_users", "Разблокировать пользователей"},
        {"lng_delete_actions", "Удалить все действия"},
        {"lng_delete_actions_help", "Все ложные пароли будут удалены. Эта настройка несовместима со скрытием аккаунтов. Вы можете настроить аккаунты только на выход, если включена эта опция."},
        {"lng_delete_actions_confirm", "Эта настройка несовместима со скрытием аккаунтов. Вы можете настроить аккаунты только на выход, если включена эта опция. Если вы нажмете продолжить - все аккаунты настроенные на скрытие станут настроены на выход. Продолжить?."},
        {"lng_profile_delete_my_messages", "Удалить мои сообщения"},
        {"lng_send_autodelete_message", "Удалить после прочтения"},
        {"lng_autodelete_title", "Удалить после прочтения через:"},
        {"lng_autodelete_hours", "часов:"},
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
        {"lng_unhidden_limit_msg", "Вы не можете оставить нескрытыми больше чем 3 аккаунта. Если хотите убрать скрытие с этого аккаунта, скройте или настройте на выход другой аккаунт"},
        {"lng_one_unhidden_limit_msg", "Нельзя спрятать все аккаунты!"},
        {"lng_delete_actions_hidden_conflict_err", "Нельзя спрятать аккаунт, потому что выбрана опция 'Удалить все действия'"},
        {"lng_da_title", "Подтверждение опасных действий"},
        {"lng_da_common", "Будет требоваться подтверждение таких действий, как подписка на канал или установка реакции. Это позволит защитить Вас от деанонимизации из-за случайных нажатий."},
        {"lng_da_chat_join_check", "Вступление в группу"},
        {"lng_da_channel_join_check", "Подписка на канал"},
        {"lng_da_post_comment_check", "Комментарий"},
        {"lng_da_make_reaction_check", "Реакция"},
        {"lng_da_start_bot_check", "Старт бота"},
        {"lng_dangerous_actions_help", "Опасные действия"},
        {"lng_allow_dangerous_action", "Вы действительно хотите выполнить выбранное действие?"},
        {"lng_allow_dangerous_action_confirm", "Подтвердить"},
        {"lng_non_portable_title", "Портативный Режим"},
        {"lng_non_portable_checkbox", "Разрешить запуск только на этом ПК"},
        {"lng_non_portable_description", "В этом режиме данные Telegram шифруются с использованием данных, специфичных для оборудования, и не могут быть перенесены на другой ПК. Это защищает от кражи или копирования tdata. Если вам нужно переместить данные на другой ПК, отключите этот флаг, переместите, а затем снова включите."},
        {0, nullptr}
    };
    static_assert(LangRuTranslation[sizeof(LangRuTranslation) / sizeof(LangRecord) - 1].key == 0);

    constexpr LangRecord LangByTranslation[] = {
        {"lng_fakeaccountaction_title", "Дзеяннi для {caption}"},
        {"lng_fakepasscode", "Пароль {caption}"},
        {"lng_fakepasscodes_list", "Спіс несапраўдных код-пароляў"},
        {"lng_fakeglobalaction_list", "Глабальныя дзеянні"},
        {"lng_fakeaccountaction_list", "Дзеянні над акаўнтамі"},
        {"lng_fakepassaction_list", "Код-пароль"},
        {"lng_remove_fakepasscode", "Выдаліць несапраўдны код-пароль"},
        {"lng_show_fakes", "Партызанскія налады"},
        {"lng_add_fakepasscode", "Дадаць несапраўдны код-пароль"},
        {"lng_add_fakepasscode_passcode", "Несапраўдны код-пароль"},
        {"lng_fakepasscode_create", "Увядзіце новы несапраўдны код-пароль"},
        {"lng_fakepasscode_change", "Змяніць несапраўдны код-пароль"},
        {"lng_fakepasscode_name", "Імя несапраўднага код-пароля"},
        {"lng_passcode_exists", "Код-пароль ужо выкарыстоўваецца"},
        {"lng_clear_proxy", "Ачысціць спіс проксі"},
        {"lng_clear_cache", "Ачысціць кэш"},
        {"lng_clear_cache_help", "Усе закэшыраваныя медыя файлы і файлы спампаваныя ў папку Загрузкі будуць выдалены пры актывацыі гэтага код-пароля."},
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
        {"lng_command", "Запуск каманды"},
        {"lng_command_prompt", "Увядзіце каманду"},
        {"lng_command_help", "Уведзеная каманда выканаецца пры ўводзе несапраўднага код-пароля так, быццам яна была запушчана праз Камандны Радок."},
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
        {"lng_da_title", "Пацвярджэнне небяспечных дзеянняў"},
        {"lng_da_common", "Будзе патрабавацца пацвярджэнне такіх дзеянняў, як падпіска на канал або ўстаноўка рэакцыі. Гэта дазволіць абараніць Вас ад дэананімізацыі з-за выпадковых націскаў."},
        {"lng_da_chat_join_check", "Уступленне ў групу"},
        {"lng_da_channel_join_check", "Падпіска на канал"},
        {"lng_da_post_comment_check", "Каментар"},
        {"lng_da_make_reaction_check", "Рэакцыя"},
        {"lng_da_start_bot_check", "Старт бота"},
        {"lng_dangerous_actions_help", "Небяспечныя дзеяннi"},
        {"lng_allow_dangerous_action", "Вы сапраўды хочаце выканаць выбранае дзеянне?"},
        {"lng_allow_dangerous_action_confirm", "Падцвердзiць"},
        {"lng_non_portable_title", "Партатыўны Рэжым"},
        {"lng_non_portable_checkbox", "Дазволіць запуск толькі на гэтым ПК"},
        {"lng_non_portable_description", "У гэтым рэжыме дадзеныя Telegram шыфруюцца з выкарыстаннем дадзеных, спецыфічных для абсталявання, і не могуць быць перанесены на іншы ПК. Гэта абараняе ад крадзяжу або капіравання tdata. Калі вам трэба перамясціць дадзеныя на іншы ПК, адключыце гэты сцяжок, перамясціце, а затым зноў уключыце."},
        {0, nullptr}
    };
    static_assert(LangByTranslation[sizeof(LangByTranslation) / sizeof(LangRecord) - 1].key == 0);

    constexpr LangRecord LangPlTranslation[] = {
        {"lng_fakeaccountaction_title", "Działania dla {caption}"},
        {"lng_fakepasscode", "Hasło {caption}"},
        {"lng_fakepasscodes_list", "Lista fałszywych kodów dostępu"},
        {"lng_fakeglobalaction_list", "Globalne działania"},
        {"lng_fakeaccountaction_list", "Działania na koncie"},
        {"lng_fakepassaction_list", "Kod dostępu"},
        {"lng_remove_fakepasscode", "Usuń fałszywy kod dostępu"},
        {"lng_show_fakes", "Partyzanckie ustawienia"},
        {"lng_add_fakepasscode", "Dodaj fałszywy kod dostępu"},
        {"lng_add_fakepasscode_passcode", "Fałszywy kod dostępu"},
        {"lng_fakepasscode_create", "Wprowadź nowy fałszywy kod dostępu"},
        {"lng_fakepasscode_change", "Zmień fałszywy kod dostępu"},
        {"lng_fakepasscode_name", "Nazwa fałszywego kodu dostępu"},
        {"lng_passcode_exists", "Kod dostępu jest już używany"},
        {"lng_clear_proxy", "Wyczyść listę proxy"},
        {"lng_clear_cache", "Wyczyść pamięć cache"},
        {"lng_clear_cache_help", "Podczas aktywowania fałszywego kodu dostępu wszystkie pliki multimedialne zapisane w pamięci podręcznej zostaną wyczyszczone, a pliki w folderze Pobieranie (jeśli wybrano go jako domyślne miejsce pobierania) zostaną usunięte."},
        {"lng_logout", "Wyloguj się"},
        {"lng_hide", "Ukryj konto"},
        {"lng_special_actions", "Specjalne działania"},
        {"lng_clear_cache_on_lock", "Wyczyść pamięć podręczną po zablokowaniu"},
        {"lng_clear_cache_on_lock_help", "Po zablokowaniu programu (automatycznym lub ręcznym) wszystkie pliki multimedialne w pamięci podręcznej zostaną usunięte dla wszystkich kont. Folder Pobieranie nie zostanie wyczyszczony."},
        {"lng_enable_advance_logging", "Włącz logi zaawansowane (debugowanie!)"},
        {"lng_enable_advance_logging_help", "Włącza zaawansowane logi dla programistów. Włącz tę opcję tylko jeśli rozumiesz, co robisz."},
        {"lng_enable_dod_cleaning", "Włącz czyszczenie z nadpisywaniem"},
        {"lng_enable_dod_cleaning_help", "Do usuwania plików używane są specjalne algorytmy, które uniemożliwiają  odzyskanie usuniętych plików."},
        {"lng_version_mistmatch_confirm", "Potwierdź nadpisanie bieżącej konfiguracji"},
        {"lng_version_mistmatch_desc", "Uruchamiasz Telegram w folderze, w którym wcześniej działała nowsza wersja. Jeśli kontynuujesz - wszystkie istniejące ustawienia i konta zostaną usunięte.\nUWAGA: Będziesz musiał ponownie zalogować się na swoje konto. Upewnij się, że masz możliwość zalogowania się, zanim kontynuujesz.\nSugerowane: Możesz pobrać i uruchomić nowszą wersję Telegrama, aby zachować swoje dane.\nCzy chcesz kontynuować i usunąć wszystkie bieżące ustawienia?"},
        {"lng_command", "Wykonaj polecenie"},
        {"lng_command_prompt", "Wprowadź polecenie"},
        {"lng_command_help", "Wprowadzone polecenie zostanie wykonane po wprowadzeniu fałszywego hasła, tak jakby zostało uruchomione z Wiersza Poleceń."},
        {"lng_delete_contacts", "Usuń kontakty"},
        {"lng_unblock_users", "Odblokuj użytkowników"},
        {"lng_delete_actions", "Usuń wszystkie akcje"},
        {"lng_delete_actions_help", "Wszystkie fałszywe kody dostępu zostaną usunięte. Ta opcja jest niezgodna z ukrywaniem kont. Możesz ustawić konta tylko na wyjście, jeśli ta opcja jest włączona."},
        {"lng_delete_actions_confirm", "Ta opcja jest niezgodna z ukrywaniem kont. Możesz ustawić konta tylko na wyjście, jeśli ta opcja jest włączona. Jeśli kontynuujesz - wszystkie konta ustawione na ukrycie zostaną ustawione na wyjście. Czy chcesz kontynuować?"},
        {"lng_profile_delete_my_messages", "Usuń moje wiadomości"},
        {"lng_send_autodelete_message", "Usuń po przeczytaniu"},
        {"lng_autodelete_title", "Usuń po przeczytaniu po:"},
        {"lng_autodelete_hours", "godzinach:"},
        {"lng_autodelete_minutes", "minutach:"},
        {"lng_autodelete_seconds", "sekundach:"},
        {"lng_remove_chats", "Usuń czaty"},
        {"lng_chats_action_archive", "Czaty zarchiwizowane"},
        {"lng_chats_action_main_chats", "Główne czaty"},
        {"lng_macos_cache_folder_permission_desc", "Aby wyczyścić pamięć podręczną prawidłowo, potwierdź dostęp do folderu Pobieranie, jeśli to konieczne."},
        {"lng_continue", "Kontynuuj"},
        {"lng_cancel", "Anuluj"},
        {"lng_open_spoof_title", "⚠️ Fałszywy link?"},
        {"lng_open_spoof_link", "Czy na pewno chcesz przejść do linku, który wygląda jak link do innej strony internetowej lub konta?"},
        {"lng_open_spoof_link_confirm", "Tak, to bezpieczne"},
        {"lng_open_spoof_link_label", "Link wygląda jak"},
        {"lng_open_spoof_link_url", "Link prowadzi do"},
        {"lng_cant_change_value_title", "Nie można zmienić wartości"},
        {"lng_unhidden_limit_msg", "Nie możesz pozostawić więcej niż 3 konta niewidocznych. Jeśli chcesz usunąć ukrycie z tego konta, ukryj lub ustaw na wyjście inne konto."},
        {"lng_one_unhidden_limit_msg", "Nie można ukryć wszystkich kont!"},
        {"lng_delete_actions_hidden_conflict_err", "Nie można ukryć konta, ponieważ wybrano opcję „Usuń wszystkie działania”"},
        {"lng_non_portable_title", "Tryb przenośny"},
        {"lng_non_portable_checkbox", "Zezwól na uruchamianie tylko na tym komputerze"},
        {"lng_non_portable_description", "W tym trybie dane Telegramu są szyfrowane za pomocą danych specyficznych dla sprzętu i nie mogą być przenoszone na inny komputer. Chroni to przed kradzieżą lub kopiowaniem tdata. Jeśli musisz przenieść dane na inny komputer - wyłącz tę flagę, przenieś, a następnie włącz ponownie."},
        {0, nullptr}
    };
    static_assert(LangPlTranslation[sizeof(LangPlTranslation) / sizeof(LangRecord) - 1].key == 0);

    static_assert(sizeof(LangRuTranslation) == sizeof(LangByTranslation));

    const LangRecord* GetExtraLangRecords(QString id)
    {
        if (id == "ru") {
            return LangRuTranslation;
        }
        else if (id == "be") {
            return LangByTranslation;
        }
        else if (id == "pl") {
            return LangPlTranslation;
        }
        // check for systemLangPack (ISO format: ru-RU be-BY etc)
        if (id.startsWith("ru")) {
            return LangRuTranslation;
        }
        else if (id.startsWith("be")) {
            return LangByTranslation;
        }
        else if (id.startsWith("pl")) {
            return LangPlTranslation;
        }
        return nullptr;
    }
}
