#include "verify.h"

namespace PTG {
namespace Verify {

    enum VerifyFlag {
        Fake,
        Scam,
        Verified
    };

    std::map<BareId, VerifyFlag> _CustomFlags;
    std::map<QString, BareId> _Name2Id;

    ChannelDataFlag ExtraChannelFlag(QString name, BareId peer_id) {
        if (auto item = _Name2Id.find(name); item != _Name2Id.end()) {
            if (item->second != peer_id) {
                // incorrect id? with old known name
                return ChannelDataFlag::PTG_Scam;
            }
        }
        if (auto item = _CustomFlags.find(peer_id); item != _CustomFlags.end()) {
            switch (item->second) {
            case Fake:
                return ChannelDataFlag::PTG_Fake;
            case Scam:
                return ChannelDataFlag::PTG_Scam;
            case Verified:
                return ChannelDataFlag::PTG_Verified;
            }
        }
        return ChannelDataFlag();
    }

    UserDataFlag ExtraUserFlag(QString name, PeerId peer_id) {
        if (auto item = _Name2Id.find(name); item != _Name2Id.end()) {
            if (item->second != peer_id.value) {
                // incorrect id? with old known name
                return UserDataFlag::PTG_Scam;
            }
        }
        if (auto item = _CustomFlags.find(peer_id.value); item != _CustomFlags.end()) {
            switch (item->second) {
            case Fake:
                return UserDataFlag::PTG_Fake;
            case Scam:
                return UserDataFlag::PTG_Scam;
            case Verified:
                return UserDataFlag::PTG_Verified;
            }
        }
        return UserDataFlag();
    }

    inline void Add(QString name, BareId id, VerifyFlag flag) {
        _CustomFlags[id] = flag;
        if (!name.isEmpty()) {
            _Name2Id[name] = id;
        }
    }

    //bool IsScam(QString name, PeerId peer_id) {
    //    if (!name.isEmpty()) {
    //        if (auto item = _Name2Id.find(name); item != _Name2Id.end()) {
    //            if (item->second != peer_id.value) {
    //                // incorrect id? with old known name
    //                return true;
    //            }
    //        }
    //    }
    //    if (auto item = _CustomFlags.find(peer_id.value); item != _CustomFlags.end()) {
    //        return (item->second == Scam);
    //    }
    //    return false;
    //}
    //bool IsFake(QString, PeerId peer_id) {
    //    if (auto item = _CustomFlags.find(peer_id.value); item != _CustomFlags.end()) {
    //        return (item->second == Fake);
    //    }
    //    return false;
    //};
    //bool IsVerified(QString name, PeerId peer_id) {
    //    if (!name.isEmpty()) {
    //        if (auto item = _Name2Id.find(name); item != _Name2Id.end()) {
    //            if (item->second != peer_id.value) {
    //                // incorrect id? with old known name
    //                return false;
    //            }
    //        }
    //    }
    //    if (auto item = _CustomFlags.find(peer_id.value); item != _CustomFlags.end()) {
    //        return (item->second == Verified);
    //    }
    //    return false;
    //};

    void Init()
    {
        Add("cpartisans_by", 1224880559, VerifyFlag::Verified);
        Add("cpartisans_security", 1164492294, VerifyFlag::Verified);
        Add("cpartisans_chat", 1297930553, VerifyFlag::Verified);
        Add("cpartisans_sec_chat", 1394174706, VerifyFlag::Verified);

        Add("cpartisans_bot", 989056630, Verified);
        Add("cpartisans_urgent_bot", 5106491425, Verified);
        Add("partisan_qa_bot", 5217087258, Verified);
        Add("cpartisans_join_bot", 5259637648, Verified);
        Add("partisan_telegram_bot", 2066143564, Verified);
        Add("facement_bot", 5817399651, Verified);
        Add("Busliniybot", 1477761243, Verified);
        Add("dns_coord_bot", 1680003670, Verified);
        Add("TGBel_bot", 5197056745, Verified);
        Add("BelarusAndUkraineBot", 5269881457, Verified);
        Add("occupant_info_bot", 5248690359, Verified);
        Add("bz_support_bot", 1764081723, Verified);
        Add("ReturnWhatStolen_bot", 1826798139, Verified);
        Add("belarusy_zarubezhja_bot", 1203525499, Verified);
        Add("zerkalo_editor", 1201956582, Verified);
        Add("euroradio_minsk", 781931059, Verified);
        Add("info_charter97", 6153646860, Verified);
        Add("SvabodaBelarus", 783723940, Verified);
        Add("Motolko_bot", 733628894, Verified);
        Add("HajunBYbot", 5233981354, Verified);
        Add("BelsatBot", 1408155238, Verified);
        Add("basta11_bot", 5088044675, Verified);
        Add("BGMnews_bot", 1254883880, Verified);
        Add("sanctionswatch_bot", 1807622699, Verified);
        Add("MKBelbot", 1235073753, Verified);
        Add("real_belarus_bot", 1610868721, Verified);
        Add("strana_official_bot", 1270329713, Verified);
        Add("KYKYmediabot", 1325073348, Verified);
        Add("belteanewsbot", 1326631129, Verified);
        Add("nick_and_mikeBot", 1578684412, Verified);
        Add("fraw_marta_bot", 1306844446, Verified);
        Add("dze_bot", 1448750362, Verified);
        Add("menskrazam_bot", 1314492187, Verified);
        Add("TheVillageBelarusBot", 1207923033, Verified);
        Add("BOR_pochta_bot", 5611596810, Verified);
        Add("BLsupport_bot", 1445915448, Verified);
        Add("zamkadombot", 1632896478, Verified);
        Add("ByProsvetVolunteerBot", 1835507118, Verified);
        Add("by_prosvet_feedback_bot", 1512107110, Verified);
        Add("fuckshtok", 1263764071, Verified);
        Add("Dmbolk_bot", 1428483199, Verified);
        Add("belhalat_bot", 1464870731, Verified);
        Add("ChatHonest_bot", 1271955412, Verified);
        Add("viasna_bot", 688209485, Verified);
        Add("stachka_by_bot", 1635921527, Verified);
        Add("ruh_connect_bot", 1715255901, Verified);
        Add("nau_by_bot", 5394894107, Verified);
        Add("kpd_by_bot", 5054426164, Verified);
        Add("hiveguide_bot", 5010895223, Verified);
        Add("belzhd_bot", 1383135185, Verified);
        Add("belzhd_editor", 1265159441, Verified);
        Add("plan_peramoga_bot", 1855819538, Verified);
        Add("bypol_chats_bot", 5955987812, Verified);
        Add("AskOffice_Bot", 1468118581, Verified);
        Add("FourOneOne4111", 1313749067, Verified);
        Add("EXOMON_support_bot", 5263169835, Verified);
        Add("balaganoff_bot", 2009270454, Verified);
        Add("mail_by", 5048219469, Verified);
        Add("Rezbat_bot", 6007283902, Verified);
        Add("pkk_reserve_bot", 5737683598, Verified);
        Add("belpolinfobot", 6508533990, Verified);
        Add("RuchBelNac_BOT", 6271362579, Verified);
        Add("rushennie_bot", 5937370959, Verified);
        Add("vb_contact_bot", 5494132715, Verified);
        Add("worldprotest_bot", 863584518, Verified);
        Add("vybor_by_bot", 1647034311, Verified);
        Add("spasisebyabot", 5507948945, Verified);
        Add("Rus_ni_peace_da_bot", 5884078727, Verified);
        Add("Yug_mopedi_bot", 5829538792, Verified);
        Add("BlackMap_bot", 1944603193, Verified);
        Add("mediazonaaby", 5927501949, Verified);
        Add("suviaz_hl", 6214140942, Verified);
        Add("CyberBeaverBot", 1768396905, Verified);
        Add("cyberpartisan_bot", 1539605834, Verified);
        Add("dissidentby_bot", 1558366823, Verified);
        Add("devby_insight_bot", 983411607, Verified);
        Add("Malanka_inbox_bot", 1091595349, Verified);
        Add("ap_narod_bot", 6616396639, Verified);
        Add("Suprativ_support_bot", 5280919337, Verified);
        Add("FindMessagesBot", 6092224989, Verified);
        Add("bysol_evacuation_bot", 5129059224, Verified);
        Add("golosby_bot", 1125659785, Verified);
        Add("mostmedia_bot", 5001919716, Verified);
        Add("CityDogBot", 728075370, Verified);
        Add("Intelligenceby_bot", 6536668537, Verified);
        Add("NovajaZiamla_bot", 1469327702, Verified);
        Add("AF_BY_bot", 2132540208, Verified);
        Add("PanHistoryjaBot", 1572171994, Verified);
        Add("NicolaiKhalezin", 406856131, Verified);
        Add("MAYDAYhelp", 1753785715, Verified);
        Add("new_grodno_bot", 1693285279, Verified);
        Add("PusovBot", 6575965275, Verified);
        Add("BudzmaSuviaz_Bot", 1669406514, Verified);
        Add("spiskov_net_bot", 1451640448, Verified);
        Add("listovki97_bot", 1497187972, Verified);
        Add("ByTribunaComBot", 960018259, Verified);
        Add("youtube_by_bot", 1984834353, Verified);
        Add("terbat_bot", 5635638840, Verified);
        Add("dns_feedback_bot", 1734398694, Verified);
        Add("motolko_NBR_bot", 1451093794, Verified);
        Add("dzechat_bot", 1099309671, Verified);
        Add("cpartisan_sanonbot", 2059952039, Scam);
        Add("cpartisans_anon_bot", 2007785891, Scam);
        Add("Bypoll", 1153790653, Scam);
        Add("predateliby", 1754069446, Scam);
        Add("face_menty_bot", 5735310739, Fake);
        Add("busliniy_bot", 1854185893, Fake);
        Add("dns_cord_bot", 1786043956, Fake);
        Add("TG_Bel_bot", 5276622916, Fake);
        Add("occupint_info_bot", 5617305774, Fake);
        Add("kdp_by_bot", 5681177489, Fake);
        Add("pian_peiramoga_bot", 5410932720, Fake);
        Add("plan_pieramoga_bot", 1839964132, Fake);
        Add("belzdh_bot", 5716598480, Fake);
        Add("tutbay_bot", 1284911026, Fake);
        Add("Matolko_bot", 5626034674, Fake);
        Add("motolkobot", 6058124728, Fake);
        Add("Motolkohelps_bot", 5747297986, Fake);
        Add("motolko_news_bot", 6035117215, Fake);
        Add("MKBbelbot", 5740485675, Fake);
        Add("cpartisan_bot", 5028668462, Fake);
        Add("ByProsvetVolunteer_Bot", 5127654526, Fake);
        Add("Busliniy_bot", 1854185893, Fake);
        Add("buslinybot", 6050276725, Fake);
        Add("busliny_bot", 6074163432, Fake);
        Add("dns_cord_bot", 1786043956, Fake);
        Add("BelarusAndUkrainieBot", 5645487088, Fake);
        Add("BelarusAndUkraine_Bot", 6293881487, Fake);
        Add("BelarusAndUkrainBot", 5606443190, Fake);
        Add("pkk_reservy_bot", 5794806573, Fake);
        Add("pkk_reserv_bot", 5543028382, Fake);
        Add("pkk_reserved_bot", 5652124632, Fake);
        Add("pkk_reserver_bot", 5731843616, Fake);
        Add("nick_and_mike_bot", 1356576596, Fake);
        Add("radiosvabodabot", 5599734900, Fake);
        Add("honestpeople_bot", 1793463571, Fake);
        Add("honest_peopIe_bot", 5017880312, Fake);
        Add("strana_svyaz_bot", 1599192547, Fake);
        Add("Haiun_BYBot", 5661955867, Fake);
        Add("bypol_proverka_bot", 5948492749, Fake);
        Add("ByPol_bot", 1407287511, Fake);
        Add("OSB_By_Pol_bot", 5840583779, Fake);
        Add("planperamogabot", 5995726427, Fake);
        Add("plan_peramogabot", 6239869956, Fake);
        Add("SupolkaBY_bot", 1375049419, Fake);
        Add("FacementBot", 403342504, Fake);
        Add("plan_piramoga_bot", 1810832442, Fake);
        Add("pieramoga_plan_bot", 1709650512, Fake);
        Add("plan_peramog_bot", 1709527783, Fake);
        Add("peramoga_plan_bot", 1532380090, Fake);
        Add("Razbat_bot", 6031551222, Fake);
        Add("belwariors", 1707221120, Fake);
        Add("BelarusAndIUkraineBot", 6292242979, Fake);
        Add("BelarussianAndUkraineBot", 5474204114, Fake);
        Add("BelarusAdnUkraineBot", 5914511754, Fake);
        Add("HajunBYanon_bot", 5630096528, Fake);
        Add("HajynBYbot", 5614673918, Fake);
        Add("HajunBEL_bot", 5757560610, Fake);
        Add("HajunB_bot", 5821054654, Fake);
        Add("HajunBYnash_bot", 5810084409, Fake);
        Add("HajunBLR_bot", 5695418211, Fake);
        Add("motolkohelp_bot", 1254143359, Fake);
        Add("MotolkohelpBot", 2110427751, Fake);
        Add("motolko_newsbot", 5700151833, Fake);
        Add("MotoIko_bot", 5698230921, Fake);
        Add("nic_and_mike_bot", 1631724675, Fake);
        Add("kpd_blr_bot", 5047547433, Fake);
        Add("kpd_b_bot", 5053420704, Fake);
        Add("RuchbelnacBot", 5779625449, Fake);
        Add("TGBelbot", 5135746255, Fake);
        Add("ruschennie", 1929789849, Fake);
        Add("Rushenniebot", 6260569674, Fake);
        Add("Paspalitaje_Rusennie_bot", 6143884311, Fake);
        Add("rushenniecz_bot", 6123656477, Fake);
        Add("Volnaja_Belaus_bot", 5634483218, Fake);
        Add("VolnayBelarus_bot", 5728606679, Fake);
        Add("golosovanie_RF", 1159302697, Fake);
        Add("insayderyurec", 1766534445, Fake);
        Add("cpartisans2020", 1730025636, Fake);
        Add("worldprotest1bot", 5423658642, Fake);
        Add("io_zerkalo", 1400869810, Fake);
        Add("TUTBAY", 1261378820, Fake);
        Add("nexta", 1864083131, Fake);
        Add("lats_bot", 1248808496, Fake);
        Add("stsikhanouskaya", 1847224666, Fake);
        Add("pulpervoi_official", 1459405938, Fake);
        Add("CabinetST", 1404319831, Fake);
        Add("naubelarus_bot", 1260250495, Fake);
        Add("mcbbelarus", 1562636546, Fake);
        Add("mkbelarys", 1877831257, Fake);
        Add("ateshua", 1956827792, Fake);
        Add("generalchereshnyaBot", 5763025616, Fake);
        Add("gnilayacherexaa", 1972480858, Fake);
        Add("bahmut_demon", 1699191195, Fake);
        Add("zhovtastrichka", 1810978803, Fake);
        Add("crimean_content", 1175048525, Fake);
        Add("partizany_crimea_bot", 6247851779, Fake);
        Add("ateshfire_bot", 6233710456, Fake);
        Add("zhovta_strichka_ua_bot", 6273841737, Fake);
        Add("hochy_zhyt", 1524487787, Fake);
        Add("hochu_zhytbot", 5731381213, Fake);
        Add("krympartizansbot", 6057323348, Fake);
        Add("gniIayacherexa", 1812284976, Fake);
        Add("zhovtastrichka_bot", 5830427793, Fake);
        Add("yellowribbon_uabot", 5857031037, Fake);
        Add("zhovta_strichkabot", 5826729656, Fake);
        Add("skrinka_pandori_lachen", 1842626385, Fake);
        Add("madyar_lachen_pishe", 1471803186, Fake);
        Add("nikolaev_vanya", 1976625090, Fake);
        Add("vanek_nikolaev1", 1805544387, Fake);
        Add("testDs1_bot", 5721687279, Fake);
        Add("evorog_gov_bot", 6069541884, Fake);
        Add("evorog_ua_bot", 5785216795, Fake);
        Add("evorog_anonim_bot", 5956713165, Fake);
        Add("evorogina_bot", 5440442548, Fake);
        Add("evorog_robot", 5324394535, Fake);
        Add("hochu_zhyut", 1719705579, Fake);
        Add("hochu_zhyt1", 1844665241, Fake);
        Add("spaslsebyabot", 5698701277, Fake);
        Add("spastisebyabot", 5727271350, Fake);
        Add("Hochu_Zhyt_Bot", 5673236403, Fake);
        Add("gdeposjar", 1566061839, Fake);
        Add("pojar_piter", 1705987207, Fake);
        Add("firemoscowandRussia", 1829232052, Fake);
        Add("Kahovskiruhbot", 6091365211, Fake);
        Add("Kakhovski_ruh_bot", 6110012702, Fake);
        Add("brdprotiv_me", 1927064019, Fake);
        Add("brdsprotiv_bot", 6221245011, Fake);
        Add("brdprotivBot", 6267293532, Fake);
        Add("brdprotiv_bot", 5734160761, Fake);
        Add("Suprotyv_brd_bpa_bot", 5760847362, Fake);
        Add("mrplsprotyv_bot", 6035643336, Fake);
        Add("peace_da_rusni_bot", 5399094229, Fake);
        Add("Rusni_peace_da_bot", 6018488147, Fake);
        Add("Rus_ni_peace_daBot", 5887363286, Fake);
        Add("Rus_ne_paece_da_bot", 5857742074, Fake);
        Add("Rusni_peaceda_bot", 6083107910, Fake);
        Add("osbbelpol_bot", 6499292937, Fake);
        Add("Black_Map_bot", 6507022757, Fake);
        Add("cpartizans_life", -1371017531, Fake);
        Add("cpartizansbot", 1137333814, Fake);
        Add("zerkalo_editoranonim_bot", 5633844875, Fake);
        Add("Supratsiv_support_bot", 2056958885, Fake);
        Add("charter97_info_bot", 6174577613, Fake);
        Add("NicolaiKhalezinBot", 5591505925, Fake);
        Add("MaydayHelpBot", 5208668406, Fake);
        Add("my_new_grodno_bot", 593287940, Fake);
        Add("nextamail", 865462332, Fake);
        Add("ChatHonestP_bot", 6171240416, Fake);
        Add("Blaganoff2022_bot", 5431821607, Fake);
        Add("zerkalo_iorobot", 6125366284, Fake);
        Add("terrorbel_bot", 5640237945, Fake);
        Add("supratsiv", 1651774110, Fake);
        Add("EURORADIOBOT", 874340248, Fake);
        Add("pIan_peramoga_exit_bot", 5799451422, Fake);
        Add("peramoga_bot", 6294664943, Fake);
        Add("BYBY_peramogaZOOk_bot", 5080158788, Fake);
        Add("help_bysol_bot", 5192128018, Fake);
        Add("judge_control_bot", 5017992144, Verified);
        Add("Stachkom_bot", 1604181100, Verified);
        Add("ruh_jurist_bot", 1800353667, Verified);
        Add("judge_controlbot", 5011634119, Fake);
        Add("evacuation_bysol_bot", 6637152080, Fake);
        Add("ReformByBot", 956492662, Fake);
        Add("belzhdor_bot", 6098545910, Fake);
        Add("beIzhd_bot", 6019626644, Fake);
        Add("RezbatBot", 6323301992, Fake);
        Add("BelAndUkrBot", 6234856292, Fake);
        Add("cpartisansbot", 1345823916, Fake);
        Add("Viasnabot", 213930779, Fake);
        Add("stachkom_azot_bot", 1565878754, Fake);
        Add("BelarusReal_bot", 5507211740, Fake);
        Add("real_belarys_bot", 5949505115, Fake);
        Add("rb_donate_bot", 1878954772, Fake);
        Add("vybory_smotribot", 1331193356, Fake);
        Add("flagshtok_info_channel_bot", 5636331501, Fake);
        Add("Charter_97_Bot", 1485288578, Fake);
        Add("HajunBY_bot", 5757764124, Fake);
        Add("beIzhd_live", 1972362876, Fake);
        Add("sac_bypol", 1404192363, Fake);
        Add("OSB_By", 5752397620, Fake);
        Add("belaruski_gajun", 1753245187, Fake);
        Add("Hajun_BL", 1489857213, Fake);
        Add("findmessages_bot", 6378147928, Fake);
        Add("euroradio_minsk_bot", 6646929530, Fake);
        Add("kpd_voice_bot", 2065381492, Verified);
        Add("odnooknobysocial_bot", 6515657015, Verified);
        Add("spasisebybot", 6560046173, Fake);
        Add("facementy_bot", 6325091001, Fake);
        Add("BelarusWithUkraineBot", 6217287060, Fake);
        Add("BeIarusAndUkraineBot", 5336631589, Fake);
        Add("Motolko_pomogi_bot", 6300793232, Fake);
        Add("Belarus_AndUkraineBot", 6319422035, Fake);
        Add("kpd_voicebot", 6288192255, Fake);
        Add("kpd_belarus", 1801304904, Fake);
        Add("judge_control11", 5273048418, Fake);
        Add("zerkalo_editor_bot", 6494105227, Fake);
        Add("realBelarus_bot", 6345790230, Fake);
        Add("real_belarusbot", 5697573854, Fake);
        Add("nextamaiI_bot", 6404725533, Fake);
        Add("HLnews1bot", 1382666687, Fake);
        Add("HrodnaBot", 318658348, Fake);
        Add("peramoha_plan_bot", 1891747638, Fake);
        Add("plan_peramoha_bot", 1887640530, Fake);
        Add("belzhd_life_bot", 6957862373, Fake);
        Add("kpd_bel", 1648063677, Fake);
        Add("kdp_by", 1563713391, Fake);
        Add("gayun_belaruski", 1723448812, Fake);
        Add("cyberpartisanBot", 6780544210, Fake);
        Add("SIivkarateIey_bot", 1428377004, Fake);
        Add("belpolinfo_bot", 6737841841, Fake);
        Add("yug_moped1_bot", 6872385843, Fake);
        Add("zerkalo_io_bot", 6702787693, Fake);
        Add("radiosvaboda_bot", 1305744997, Fake);
        Add("bydeanon", 1203784167, Fake);
        Add("terrorystibelarusi", 1353833851, Fake);
        Add("karatelibelarusi", 1313705214, Verified);
        Add("karatelibelarusii", 1265537364, Fake);
        Add("karatelibelarusilive", 1332577985, Fake);
        Add("karatelibelarusi2", 1365329737, Fake);
        Add("karatelibelarusiii", 1371722304, Fake);
        Add("karatelibelarusiofficial", 1220357869, Fake);
        Add("terroristibelarusi", 1340697689, Fake);
        Add("karatelibelarusi_rezerv", 1386027427, Fake);
        Add("karatelibelarusil", 1808365173, Fake);
        Add("karatelibelarusyk", 1491605751, Fake);
        Add("karatelibelarusi_live", 1490897795, Fake);
        Add("terroristybelarusi", 1169050105, Fake);
        Add("BlackBookBelarus", 1395400220, Verified);
        Add("RezbattBot", 6691593629, Fake);
        Add("Rezbatt_bot", 6634255704, Fake);
        Add("cpartisanybot", 1091579513, Fake);
        Add("stop_extremism_bot", 6544599844, Scam);
        Add("cpartisens_bot", 6979053691, Fake);
        Add("CyberRebel_bot", 5214633844, Fake);
        Add("cpartisanBot", 6389389740, Fake);
        Add("BelarusAnd_UkraineBot", 6086769556, Fake);
        Add("Belsat_TV_bot", 6648487127, Fake);
        Add("BelPolice_bot", 6432197187, Fake);
        Add("belpol_infobot", 6788221696, Fake);
        Add("Hajun_bybot", 5192559535, Fake);
        Add("Gajun_BY", 1855539095, Fake);
        Add("berarusskigajun", 1881366070, Fake);
        Add("westernbattalion_bot", 6355597990, Fake);
        Add("belarmya_bot", 6367205491, Fake);
        Add("bel_armyBot", 6156784863, Fake);
        Add("cyberpartysan_bot", 6022708348, Fake);
        Add("evocationinfo_bot", 5550488358, Fake);
        Add("StopPropogandaPost", 6256348629, Scam);
        Add("StopLykaBot", 6031007106, Scam);
        Add("vr_kr_bot", 6719512597, Verified);
        Add("bel_army_bot", 6441299804, Fake);
        Add("belzdh_live", 1523106534, Fake);
        Add("zerkalo_editor4bot", 6413869971, Fake);
        Add("real_belarus1_bot", 5656963306, Fake);
        Add("belarmy_bot", 6683894928, Verified);
        Add("NaVinYM", 5064239146, Verified);
        Add("Licvinybot", 5535531128, Verified);
        Add("pkk_feedback_bot", 5760125370, Verified);
        Add("pkk_resarve_bot", 5606345142, Fake);
        Add("ocupant_info_bot", 5336263870, Fake);
        Add("wr_kr_bot", 6821754020, Fake);
        Add("facements_Bot", 6712027848, Fake);
        Add("rushenniecz", 5929518307, Fake);
        Add("zerkaloio_rss_bot", 2074846938, Fake);
        Add("zerkalo_io_moderator_bot", 5046699873, Fake);
        Add("real_belarus_admin_bot", 1698748273, Fake);
        Add("realnaya_belarus_bot", 6447629262, Fake);
        Add("belarmybot", 444567128, Fake);
        Add("BgmNewsBot", 5261048158, Fake);
        Add("BGMNEWS_autopost_bot", 5886895222, Fake);
        Add("cpartisans_joinbot", 6539116883, Fake);
        Add("Post_rudzik_bot", 5440682180, Verified);
        Add("FeedbackDvizhBot", 1558674272, Verified);
        Add("CheckFakesBot", 6778178263, Verified);
        Add("belsat462_bot", 5827724682, Fake);
        Add("belsat_555_bot", 6928469097, Fake);
        Add("belsat_stat_bot", 1160258188, Fake);
        Add("BelsatSuviazBot", 1273848017, Fake);
        Add("belsat_post_bot", 747763086, Fake);
        Add("terbatl_bot", 5656374906, Fake);
        Add("mkbelarusbot", 410926647, Fake);
        Add("cpartisans_security_bot", 6713378551, Fake);
        Add("hajun_by_035_bot", 6543263711, Fake);
        Add("hajun_by_737_bot", 6951447059, Fake);
        Add("usylukashenko_bot", 6781371452, Fake);
        Add("Za_mkad_bot", 6545941247, Fake);
        Add("zamkadomby_bot", 6825716216, Fake);
        Add("nik_i_maik_telegramm_bot", 6524453586, Fake);
        Add("NEXTA_live_bot", 1110705319, Fake);
        Add("nexta_tv_bot", 937919810, Fake);
        Add("nexta_tv_701_bot", 6976558444, Fake);
        Add("Nexta_tvBot", 6920173090, Fake);
        Add("luxta_tv_bot", 5124239624, Fake);
        Add("nextalivebot", 1351727889, Fake);
        Add("charter97_org_bot", 6810646200, Fake);
        Add("REFORMBY_bot", 6681631955, Fake);
        Add("reformby_news_bot", 1439787920, Fake);
        Add("hiveguiide_bot", 5016143831, Fake);
        Add("hiveguidance_bot", 5072733585, Fake);
        Add("kpd_bysol_bot", 6829600851, Fake);
        Add("belwarriors_bot", 5178091436, Fake);
        Add("nashaniva_bot", 6807026412, Fake);
        Add("NashaNivaNewBot", 1342645171, Fake);
        Add("nashaniva_056_bot", 6761428860, Fake);
        Add("nashaniva_775_bot", 6864086332, Fake);
        Add("plan_peremoga_bot", 5816692126, Fake);
        Add("CabinetST_bot", 1532211584, Fake);
        Add("belzhd_live_bot", 6654699772, Fake);
        Add("menskrazam_by_bot", 6807448358, Fake);
        Add("tsikhanouskayabot", 1379784701, Fake);
        Add("honest_grodno_1bot", 5008958540, Fake);
        Add("honest_grodnobot", 5039140782, Fake);
        Add("honest_grodno_bot", 5014461094, Fake);
        Add("balaganoffnews_bot", 6523603434, Fake);
        Add("bnkbel_236_bot", 6430079024, Fake);
        Add("basta21_bot", 1442812572, Fake);
        Add("menskrazam_by_bot", 6807448358, Fake);
        Add("tribuna_by_bot", 6890210257, Fake);
        Add("realnaiabelarus_bot", 6364302991, Fake);
        Add("uniannetbot", 507131766, Fake);
        Add("ViasnaSOS_bot", 6967012612, Fake);
        Add("", 6674789527, Scam);
        Add("facementi_bot", 6710700188, Fake);
        Add("cprtisans_bot", 6777854116, Fake);
        Add("CMEPTb_OMOHAM", 6285548029, Verified);
        Add("OMOH_COCET", 5882623416, Verified);
        Add("HET_OMOHAM", 6046884357, Verified);
        Add("OHT_BPET", 1654964003, Verified);
        Add("CKOPO_TAPAKAHA_HE_CTAHET", 1853028137, Verified);
        Add("Paspalitae_Rushanne_bot", 5330873387, Fake);
        Add("cpartisansDeanon_bot", 6372645586, Fake);
        Add("MotolkoPomogi_bot", 6978755675, Fake);
        Add("dzie_bot", 6611710821, Fake);
        Add("test2891hdisbot", 6874662121, Fake);
        Add("findmessagesbot_bot", 6800190981, Scam);
        Add("Journalist_Zerkalo_io", 6295427716, Fake);
        Add("BelarusZaMkadomBot", 6703519634, Fake);
        Add("ZaMKADOM_BelarusBot", 6850913257, Fake);
        Add("radiosvobodabelBot", 6419287646, Fake);
        Add("ViasnaS0S_bot", 6951278709, Fake);
        Add("Vybory_by_viasna_bot", 6757590263, Fake);
        Add("Viasna_vybary_Bot", 6736754754, Fake);
        Add("ViasnaSOS_BY_bot", 6578343275, Fake);
        Add("honestpeople_by_bot", 6901684038, Fake);
        Add("Guardian1444", 1424639823, Scam);
        Add("help_bysol_bot", 5192128018, Verified);
        Add("spasisebya_bot", 6622656537, Fake);
        Add("belpolINF0_bot", 6453496618, Fake);
        Add("belpollbot", 6817538472, Fake);
        Add("PolkKalinowcy_bot", 5673228263, Fake);
        Add("Polk_KalinovskogoBot", 6635153175, Fake);
        Add("BelarusZaUkraineBot", 6754161621, Fake);
        Add("Pouk_Kalinouskaga_bot", 6979256817, Fake);
        Add("BelarusAnd_Ukraine_Bot", 6626214300, Fake);
        Add("Tsikhaya_out_bot", 5928686408, Fake);
        Add("viasna96_006_bot", 6694495669, Fake);
        Add("KarateliBLR_bot", 1269004905, Fake);
        Add("SIivkarateIeyBot", 6874881595, Fake);
        Add("paspalitae_rush_bot", 6611801654, Fake);
        Add("belsat_151_bot", 6717810122, Fake);
        Add("Kiber_Partizan_bot", 5955937353, Fake);
        Add("BelarusAndUkraineRobot", 6661924593, Fake);
        Add("plan_peramogaBot", 6888177288, Fake);
        Add("BelarusAndUkraineRobot", 6661924593, Fake);
        Add("plan_peramogaBot", 6888177288, Fake);
        Add("Kadravy_rezerv", 2090787928, Fake);
        Add("pr_belarus", 1811327014, Verified);
        Add("Kadravy_rezerv_bot", 6751455066, Fake);
        Add("Tsapkalo_VV_bot", 5566046216, Fake);
        Add("rushenniye_bot", 6626296635, Fake);
        Add("zerkalo_edit_Bot", 6814233954, Fake);
        Add("pIanperamoga_bot", 5639462175, Fake);
        Add("real_belarusBot", 6923026796, Fake);
        Add("BeloruseAndUkraineBot", 6964968575, Fake);
        Add("BelarusAndUkrainBot", 6781623830, Fake);
        Add("belamovaBot", 6201856102, Fake);
        Add("KYKYmedia_bot", 6578626477, Fake);
        Add("SvabodaBelarus_robot", 6933357232, Fake);
        Add("c97bot", 546217165, Fake);
        Add("Charter97info_bot", 6017932167, Fake);
        Add("charter97org_bot", 1372008778, Fake);
        Add("cpartisany_bot", 6758643748, Fake);
        Add("cpartisany_join_bot", 6046290945, Fake);
        Add("BlackMapBot", 6959450210, Fake);
        Add("HajunBELbot", 6870112855, Fake);
        Add("STsikhanouskaya_bot", 5455803078, Fake);
        Add("cyberpartisans_bot", 6972861970, Fake);
        Add("cyberpartisany_bot", 6786427818, Fake);
        Add("dzikaeplvnBot", 6416032190, Fake);
        Add("dzikaeplvn_Bot", 6467482183, Fake);
        Add("belarus_blacklistBot", 6849786648, Fake);
        Add("nextamailbot", 6874526605, Fake);
        Add("VokaZmagara_Bot", 6839990682, Fake);
        Add("kpd_byBot", 6704210310, Fake);
        Add("CKOPO_TAPAKAHA_HE_CTAHE", 727134712, Fake);
        Add("ChatHohest_bot", 6576670446, Fake);
        Add("Latyshko_bot", 5470004920, Fake);
        Add("belarmy_EU_bot", 6708161018, Fake);
        Add("Rezbat_EU_bot", 6442784940, Fake);
        Add("BelwarriorsDE", 1808474987, Fake);
        Add("Motolkolbot", 6931218645, Fake);
        Add("dhze_bot", 6946687105, Fake);
        Add("Elenazhivoglod", 6224130729, Fake);
        Add("Lenazhivoglod", 290203926, Verified);
        Add("buro_editors", 6876697454, Verified);
        Add("PYTY_DOMOY", 1989826086, Verified);
        Add("put_domoj_bot", 6657241135, Verified);
        Add("putdomo", 1559445028, Verified);
        Add("PUTY_DOMOY", 1993018754, Fake);
        Add("PYTY_DOMOY_OPEN", 2003693727, Fake);
        Add("beiwarrior", 1820516995, Fake);
        Add("Rezbat_Bel_bot", 6799600345, Fake);
        Add("hiveguidebot", 7188690031, Fake);
        Add("kpd_by", 1673348449, Verified);
        Add("hlveguide_bot", 6910198017, Fake);
        Add("zerkalo_io_news_bot", 6250321372, Fake);
        Add("VokoZmagara_bot", 6924350073, Fake);
        Add("Face_mentBot", 6470443682, Fake);
        Add("Polk_Kalinoyskaga_bot", 6844086708, Fake);
        Add("Polk_Kalinoyskaga", 6085402831, Fake);
        Add("flagstok_gomel_bot", 6788137747, Fake);
        Add("realnaya_belarus_telegramm_bot", 6915711527, Fake);
        Add("BelPoliceBot", 6844673349, Fake);
        Add("sp_telegraf", 6241885336, Verified);
        Add("news_zerkalo_io_bot", 6702249971, Fake);
        Add("BelArmyRecruitBot", 6951426314, Fake);
        Add("terorbel", -1867767006, Fake);
        Add("Poldenprotiv_bot", 7010150701, Fake);
        Add("poldenprotivputina_bot", 7198710704, Fake);
        Add("poldeninfo_bot", 6958523707, Fake);
        Add("plan_peramoga2_bot", 7166243277, Fake);
        Add("bypol_officialy", -2064034663, Fake);
        Add("Bypol_peramoga2", 6838798287, Fake);
        Add("Hajunblrbot", 6776986977, Fake);
        Add("belpol_info_bot", 7054021710, Fake);
        Add("HaiunBYbot", 6653512373, Fake);
        Add("Kadrovyi_rezerv_bot", 6715883696, Fake);
        Add("belzdh_robot", 6886369998, Fake);
        Add("belzhdor", -1702365388, Fake);
        Add("belcabinet_bot", 5703511335, Verified);
        Add("belarusisvetinfo", 6438311047, Verified);
        Add("cpartisans_joins_bot", 7082640122, Fake);
        Add("partisans_qa_bot", 7018375337, Fake);
        Add("occupants_info_bot", 7182210599, Fake);
        Add("Bysliniybot", 5657739381, Fake);
        Add("Chathonestbot", 6849345270, Fake);
        Add("zerkalo_novosti_bot", 6466350613, Fake);
        Add("zerkalo_bai_novosti_tut_bot", 6962067565, Fake);
        Add("BelandUk_bot", 7167673428, Fake);
        Add("OPkastus_bot", 6262094593, Fake);
        Add("kalinouski_applications_bot", 7067512611, Fake);
        Add("OBTerror_bot", 6652576307, Fake);
        Add("bat_terror_bot", 5690252050, Fake);
        Add("KYKYorg_bot", 639308606, Fake);
        Add("nextaGmail_bot", 7012589505, Fake);
        Add("realbelarusbot", 6520392173, Fake);
        Add("zerkalo_edition_bot", 7019492115, Fake);
        Add("tutby_editorbot", 1376764515, Fake);
        Add("Kalinoyskaga_Polk_bot", 6356547138, Fake);
        Add("belarmy_official_bot", 6619667711, Fake);
        Add("belarmi_bot", 7065899488, Fake);
        Add("beIarmy_bot", 7010768070, Fake);
        Add("PolkKalinovskogoBot", 6834159504, Fake);
        Add("PolkKalinovskogo_Bot", 6754628653, Fake);
        Add("HajunBYbbot", 6882593470, Fake);
        Add("cpartisanes_bot", 6797520509, Fake);
        Add("cpartisansjoinbot", 7021585597, Fake);
    }

}
}
