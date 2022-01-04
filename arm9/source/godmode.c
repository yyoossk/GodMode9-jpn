#include "godmode.h"
#include "paint9.h"
#include "memmap.h"
#include "support.h"
#include "ui.h"
#include "swkbd.h"
#include "hid.h"
#include "swkbd.h"
#include "touchcal.h"
#include "fs.h"
#include "utils.h"
#include "nand.h"
#include "gamecart.h"
#include "virtual.h"
#include "vcart.h"
#include "game.h"
#include "disadiff.h"
#include "unittype.h"
#include "entrypoints.h"
#include "bootfirm.h"
#include "png.h"
#include "timer.h"
#include "rtc.h"
#include "power.h"
#include "vram0.h"
#include "i2c.h"
#include "pxi.h"

#ifndef N_PANES
#define N_PANES 3
#endif

#define COLOR_TOP_BAR   (PERM_RED ? COLOR_RED : PERM_ORANGE ? COLOR_ORANGE : PERM_BLUE ? COLOR_BRIGHTBLUE : \
                         PERM_YELLOW ? COLOR_BRIGHTYELLOW : PERM_GREEN ? COLOR_GREEN : COLOR_WHITE)

#define BOOTPAUSE_KEY   (BUTTON_R1|BUTTON_UP)
#define BOOTMENU_KEY    (BUTTON_R1|BUTTON_LEFT)
#define BOOTFIRM_PATHS  "0:/bootonce.firm", "0:/boot.firm", "1:/boot.firm"
#define BOOTFIRM_TEMPS  0x1 // bits mark paths as temporary

#ifdef SALTMODE // ShadowHand's own bootmenu key override
#undef  BOOTMENU_KEY
#define BOOTMENU_KEY    BUTTON_START
#endif


typedef struct {
    char path[256];
    u32 cursor;
    u32 scroll;
} PaneData;


u32 BootFirmHandler(const char* bootpath, bool verbose, bool delete) {
    char pathstr[UTF_BUFFER_BYTESIZE(32)];
    TruncateString(pathstr, bootpath, 32, 8);

    size_t firm_size = FileGetSize(bootpath);
    if (!firm_size) return 1;
    if (firm_size > FIRM_MAX_SIZE) {
        if (verbose) ShowPrompt(false, "%s\nFIRMが大きすぎて起動できません", pathstr); // unlikely
        return 1;
    }

    if (verbose && !ShowPrompt(true, "%s (%dkB)\n警告: 信頼できないソースから\nFIRMを起動しないで下さい。\n \nBoot FIRM?",
        pathstr, firm_size / 1024))
        return 1;

    void* firm = (void*) malloc(FIRM_MAX_SIZE);
    if (!firm) return 1;
    if ((FileGetData(bootpath, firm, firm_size, 0) != firm_size) ||
        !IsBootableFirm(firm, firm_size)) {
        if (verbose) ShowPrompt(false, "%s\n起動可能なFIRMではありません。", pathstr);
        free(firm);
        return 1;
    }

    // encrypted firm handling
    FirmSectionHeader* arm9s = FindFirmArm9Section(firm);
    if (!arm9s) return 1;

    FirmA9LHeader* a9l = (FirmA9LHeader*)(void*) ((u8*) firm + arm9s->offset);
    if (verbose && (ValidateFirmA9LHeader(a9l) == 0) &&
        ShowPrompt(true, "%s\nFIRMは暗号化されています。\n \nブート前に復号化しますか?", pathstr) &&
        (DecryptFirmFull(firm, firm_size) != 0)) {
        free(firm);
        return 1;
    }

    // unsupported location handling
    char fixpath[256] = { 0 };
    if (verbose && (*bootpath != '0') && (*bootpath != '1')) {
        const char* optionstr[2] = { "以下にコピーしてください。" OUTPUT_PATH "/temp.firm", "起動してみる" };
        u32 user_select = ShowSelectPrompt(2, optionstr, "%s\n警告: サポートされていない場所\nからの起動を試みています。", pathstr);
        if (user_select == 1) {
            FileSetData(OUTPUT_PATH "/temp.firm", firm, firm_size, 0, true);
            bootpath = OUTPUT_PATH "/temp.firm";
        } else if (!user_select) bootpath = "";
    }

    // fix the boot path ("sdmc"/"nand" for Luma et al, hacky af)
    if ((*bootpath == '0') || (*bootpath == '1'))
        snprintf(fixpath, 256, "%s%s", (*bootpath == '0') ? "sdmc" : "nand", bootpath + 1);
    else strncpy(fixpath, bootpath, 256);
    fixpath[255] = '\0';

    // boot the FIRM (if we got a proper fixpath)
    if (*fixpath) {
        if (delete) PathDelete(bootpath);
        DeinitExtFS();
        DeinitSDCardFS();
        PXI_DoCMD(PXICMD_LEGACY_BOOT, NULL, 0);
        PXI_Barrier(PXI_FIRMLAUNCH_BARRIER);
        BootFirm((FirmHeader*) firm, fixpath);
        while(1);
    }

    // a return was not intended
    free(firm);
    return 1;
}

u32 SplashInit(const char* modestr) {
    u64 splash_size;
    u8* splash = FindVTarFileInfo(VRAM0_SPLASH_PNG, &splash_size);
    const char* namestr = FLAVOR " " VERSION;
    const char* loadstr = "起動中...";
    const u32 pos_xb = 10;
    const u32 pos_yb = 10;
    const u32 pos_xu = SCREEN_WIDTH_BOT - 10 - GetDrawStringWidth(loadstr);
    const u32 pos_yu = SCREEN_HEIGHT - 10 - GetDrawStringHeight(loadstr);

    ClearScreenF(true, true, COLOR_STD_BG);

    if (splash) {
        u32 splash_width, splash_height;
        u16* bitmap = PNG_Decompress(splash, splash_size, &splash_width, &splash_height);
        if (bitmap) {
            DrawBitmap(TOP_SCREEN, -1, -1, splash_width, splash_height, bitmap);
            free(bitmap);
        }
    } else {
        DrawStringF(TOP_SCREEN, 10, 10, COLOR_STD_FONT, COLOR_TRANSPARENT, "(" VRAM0_SPLASH_PNG " 見当たらない)");
    }

    if (modestr) DrawStringF(TOP_SCREEN, SCREEN_WIDTH_TOP - 10 - GetDrawStringWidth(modestr),
        SCREEN_HEIGHT - 10 - GetDrawStringHeight(modestr), COLOR_STD_FONT, COLOR_TRANSPARENT, modestr);

    DrawStringF(BOT_SCREEN, pos_xb, pos_yb, COLOR_STD_FONT, COLOR_STD_BG, "%s\n%*.*s\n%s\n \n \n%s\n%s\n \n%s\n%s",
        namestr, strnlen(namestr, 64), strnlen(namestr, 64),
        "--------------------------------", "https://github.com/d0k3/GodMode9",
        "リリース:", "https://github.com/d0k3/GodMode9/releases/", // this won't fit with a 8px width font
        "Hourlies:", "https://d0k3.secretalgorithm.com/");
    DrawStringF(BOT_SCREEN, pos_xu, pos_yu, COLOR_STD_FONT, COLOR_STD_BG, loadstr);
    DrawStringF(BOT_SCREEN, pos_xb, pos_yu, COLOR_STD_FONT, COLOR_STD_BG, "構築: " DBUILTL);

    return 0;
}

#ifndef SCRIPT_RUNNER
static DirStruct* current_dir = NULL;
static DirStruct* clipboard   = NULL;
static PaneData* panedata     = NULL;

void GetTimeString(char* timestr, bool forced_update, bool full_year) {
    static DsTime dstime;
    static u64 timer = (u64) -1; // this ensures we don't check the time too often
    if (forced_update || (timer == (u64) -1) || (timer_sec(timer) > 30)) {
        get_dstime(&dstime);
        timer = timer_start();
    }
    if (timestr) snprintf(timestr, 31, "%s%02lX-%02lX-%02lX %02lX:%02lX", full_year ? "20" : "",
        (u32) dstime.bcd_Y, (u32) dstime.bcd_M, (u32) dstime.bcd_D, (u32) dstime.bcd_h, (u32) dstime.bcd_m);
}

void CheckBattery(u32* battery, bool* is_charging) {
    if (battery) {
        static u32 battery_l = 0;
        static u64 timer_b = (u64) -1; // this ensures we don't check too often
        if ((timer_b == (u64) -1) || (timer_sec(timer_b) >= 120)) {
            battery_l = GetBatteryPercent();
            timer_b = timer_start();
        }
        *battery = battery_l;
    }

    if (is_charging) {
        static bool is_charging_l = false;
        static u64 timer_c = (u64) -1;
        if ((timer_c == (u64) -1) || (timer_sec(timer_c) >= 1)) {
            is_charging_l = IsCharging();
            timer_c = timer_start();
        }
        *is_charging = is_charging_l;
    }
}

void DrawBatteryBitmap(u16* screen, u32 b_x, u32 b_y, u32 width, u32 height) {
    const u16 color_outline = COLOR_BLACK;
    const u16 color_inline = COLOR_LIGHTGREY;
    const u16 color_inside = COLOR_LIGHTERGREY;
    const u16 color_bg = COLOR_TRANSPARENT;

    if ((width < 8) || (height < 6)) return;

    u32 battery;
    bool is_charging;
    CheckBattery(&battery, &is_charging);

    u16 color_battery = (is_charging) ? COLOR_BATTERY_CHARGING :
        (battery > 70) ? COLOR_BATTERY_FULL : (battery > 30) ? COLOR_BATTERY_MEDIUM : COLOR_BATTERY_LOW;
    u32 nub_size = (height < 12) ? 1 : 2;
    u32 width_inside = width - 4 - nub_size;
    u32 width_battery = (battery >= 100) ? width_inside : ((battery * width_inside) + 50) / 100;

    for (u32 y = 0; y < height; y++) {
        const u32 mirror_y = (y >= (height+1) / 2) ? height - 1 - y : y;
        for (u32 x = 0; x < width; x++) {
            const u32 rev_x = width - x - 1;
            u16 color = 0;
            if (mirror_y == 0) color = (rev_x >= nub_size) ? color_outline : color_bg;
            else if (mirror_y == 1) color = ((x == 0) || (rev_x == nub_size)) ? color_outline : (rev_x < nub_size) ? color_bg : color_inline;
            else if (mirror_y == 2) color = ((x == 0) || (rev_x <= nub_size)) ? color_outline : ((x == 1) || (rev_x == (nub_size+1))) ? color_inline : color_inside;
            else color = ((x == 0) || (rev_x == 0)) ? color_outline : ((x == 1) || (rev_x <= (nub_size+1))) ? color_inline : color_inside;
            if ((color == color_inside) && (x < (2 + width_battery))) color = color_battery;
            if (color != color_bg) DrawPixel(screen, b_x + x, b_y + y, color);
        }
    }
}

void DrawTopBar(const char* curr_path) {
    const u32 bartxt_start = (FONT_HEIGHT_EXT >= 10) ? 1 : (FONT_HEIGHT_EXT >= 7) ? 2 : 3;
    const u32 bartxt_x = 2;
    const u32 len_path = SCREEN_WIDTH_TOP - 120;
    char tempstr[UTF_BUFFER_BYTESIZE(63)];

    // top bar - current path
    DrawRectangle(TOP_SCREEN, 0, 0, SCREEN_WIDTH_TOP, 12, COLOR_TOP_BAR);
    if (*curr_path) TruncateString(tempstr, curr_path, min(63, len_path / FONT_WIDTH_EXT), 8);
    else snprintf(tempstr, 16, "[root]");
    DrawStringF(TOP_SCREEN, bartxt_x, bartxt_start, COLOR_STD_BG, COLOR_TOP_BAR, "%s", tempstr);
    bool show_time = true;

    #ifdef SHOW_FREE
    if (*curr_path) { // free & total storage
        const u32 bartxt_rx = SCREEN_WIDTH_TOP - (19*FONT_WIDTH_EXT) - bartxt_x;
        char bytestr0[32];
        char bytestr1[32];
        DrawStringF(TOP_SCREEN, bartxt_rx, bartxt_start, COLOR_STD_BG, COLOR_TOP_BAR, "%19.19s", "読み込み中...");
        FormatBytes(bytestr0, GetFreeSpace(curr_path));
        FormatBytes(bytestr1, GetTotalSpace(curr_path));
        snprintf(tempstr, 64, "%s/%s", bytestr0, bytestr1);
        DrawStringF(TOP_SCREEN, bartxt_rx, bartxt_start, COLOR_STD_BG, COLOR_TOP_BAR, "%19.19s", tempstr);
        show_time = false;
    }
    #elif defined MONITOR_HEAP
    if (true) { // allocated mem
        const u32 bartxt_rx = SCREEN_WIDTH_TOP - (9*FONT_WIDTH_EXT) - bartxt_x;
        char bytestr[32];
        FormatBytes(bytestr, mem_allocated());
        DrawStringF(TOP_SCREEN, bartxt_rx, bartxt_start, COLOR_STD_BG, COLOR_TOP_BAR, "%9.9s", bytestr);
        show_time = false;
    }
    #endif

    if (show_time) { // clock & battery
        const u32 battery_width = 16;
        const u32 battery_height = 9;
        const u32 battery_x = SCREEN_WIDTH_TOP - battery_width - bartxt_x;
        const u32 battery_y = (12 - battery_height) / 2;
        const u32 clock_x = battery_x - (15*FONT_WIDTH_EXT);

        char timestr[32];
        GetTimeString(timestr, false, false);
        DrawStringF(TOP_SCREEN, clock_x, bartxt_start, COLOR_STD_BG, COLOR_TOP_BAR, "%14.14s", timestr);
        DrawBatteryBitmap(TOP_SCREEN, battery_x, battery_y, battery_width, battery_height);
    }
}

void DrawUserInterface(const char* curr_path, DirEntry* curr_entry, u32 curr_pane) {
    const u32 n_cb_show = 8;
    const u32 info_start = (MAIN_SCREEN == TOP_SCREEN) ? 18 : 2; // leave space for the topbar when required
    const u32 instr_x = (SCREEN_WIDTH_MAIN - (34*FONT_WIDTH_EXT)) / 2;
    const u32 len_info = (SCREEN_WIDTH_MAIN - ((SCREEN_WIDTH_MAIN >= 400) ? 80 : 20)) / 2;
    const u32 str_len_info = min(63, len_info / FONT_WIDTH_EXT);
    char tempstr[UTF_BUFFER_BYTESIZE(str_len_info)];

    static u32 state_prev = 0xFFFFFFFF;
    u32 state_curr =
        ((*curr_path) ? (1<<0) : 0) |
        ((clipboard->n_entries) ? (1<<1) : 0) |
        ((CheckSDMountState()) ? (1<<2) : 0) |
        ((GetMountState()) ? (1<<3) : 0) |
        ((GetWritePermissions() > PERM_BASE) ? (1<<4) : 0) |
        (curr_pane<<5);

    if (state_prev != state_curr) {
        ClearScreenF(true, false, COLOR_STD_BG);
        state_prev = state_curr;
    }

    // left top - current file info
    if (curr_pane) snprintf(tempstr, 63, "PANE #%lu", curr_pane);
    else snprintf(tempstr, 63, "現在");
    DrawStringF(MAIN_SCREEN, 2, info_start, COLOR_STD_FONT, COLOR_STD_BG, "[%s]", tempstr);
    // file / entry name
    ResizeString(tempstr, curr_entry->name, str_len_info, 8, false);
    u32 color_current = COLOR_ENTRY(curr_entry);
    DrawStringF(MAIN_SCREEN, 4, info_start + 12, color_current, COLOR_STD_BG, "%s", tempstr);
    // size (in Byte) or type desc
    if (curr_entry->type == T_DIR) {
        ResizeString(tempstr, "(dir)", str_len_info, 8, false);
    } else if (curr_entry->type == T_DOTDOT) {
        snprintf(tempstr, 21, "%20s", "");
    } else if (curr_entry->type == T_ROOT) {
        int drvtype = DriveType(curr_entry->path);
        char drvstr[32];
        snprintf(drvstr, 31, "(%s%s)",
            ((drvtype & DRV_SDCARD) ? "SDカード" : (drvtype & DRV_RAMDRIVE) ? "RAMドライブ" : (drvtype & DRV_GAME) ? "ゲーム" :
            (drvtype & DRV_SYSNAND) ? "SysNAND" : (drvtype & DRV_EMUNAND) ? "EmuNAND" : (drvtype & DRV_IMAGE) ? "画像" :
            (drvtype & DRV_XORPAD) ? "XORpad" : (drvtype & DRV_MEMORY) ? "メモリ" : (drvtype & DRV_ALIAS) ? "通称" :
            (drvtype & DRV_CART) ? "ゲームカード" : (drvtype & DRV_VRAM) ? "VRAM" : (drvtype & DRV_SEARCH) ? "検索" :
            (drvtype & DRV_TITLEMAN) ? "タイトルマネージャー" : ""),
            ((drvtype & DRV_FAT) ? " FAT" : (drvtype & DRV_VIRTUAL) ? " バーチャル" : ""));
        ResizeString(tempstr, drvstr, str_len_info, 8, false);
    } else {
        char numstr[32];
        char bytestr[32];
        FormatNumber(numstr, curr_entry->size);
        snprintf(bytestr, 31, "%s Byte", numstr);
        ResizeString(tempstr, bytestr, str_len_info, 8, false);
    }
    DrawStringF(MAIN_SCREEN, 4, info_start + 12 + 10, color_current, COLOR_STD_BG, tempstr);
    // path of file (if in search results)
    if ((DriveType(curr_path) & DRV_SEARCH) && strrchr(curr_entry->path, '/')) {
        char dirstr[256];
        strncpy(dirstr, curr_entry->path, 256);
        *(strrchr(dirstr, '/')+1) = '\0';
        ResizeString(tempstr, dirstr, str_len_info, 8, false);
        DrawStringF(MAIN_SCREEN, 4, info_start + 12 + 10 + 10, color_current, COLOR_STD_BG, "%s", tempstr);
    } else {
        ResizeString(tempstr, "", str_len_info, 8, false);
        DrawStringF(MAIN_SCREEN, 4, info_start + 12 + 10 + 10, color_current, COLOR_STD_BG, "%s", tempstr);
    }

    // right top - clipboard
    DrawStringF(MAIN_SCREEN, SCREEN_WIDTH_MAIN - len_info, info_start, COLOR_STD_FONT, COLOR_STD_BG, "%*s",
        len_info / FONT_WIDTH_EXT, (clipboard->n_entries) ? "[クリップボード]" : "");
    for (u32 c = 0; c < n_cb_show; c++) {
        u32 color_cb = COLOR_ENTRY(&(clipboard->entry[c]));
        ResizeString(tempstr, (clipboard->n_entries > c) ? clipboard->entry[c].name : "", str_len_info, 8, true);
        DrawStringF(MAIN_SCREEN, SCREEN_WIDTH_MAIN - len_info - 4, info_start + 12 + (c*10), color_cb, COLOR_STD_BG, "%s", tempstr);
    }
    *tempstr = '\0';
    if (clipboard->n_entries > n_cb_show) snprintf(tempstr, 60, "+ %lu その他", clipboard->n_entries - n_cb_show);
    DrawStringF(MAIN_SCREEN, SCREEN_WIDTH_MAIN - len_info - 4, info_start + 12 + (n_cb_show*10), COLOR_DARKGREY, COLOR_STD_BG,
        "%*s", len_info / FONT_WIDTH_EXT, tempstr);

    // bottom: instruction block
    char instr[512];
    snprintf(instr, 512, "%s\n%s%s%s%s%s%s%s%s",
        FLAVOR " " VERSION, // generic start part
        (*curr_path) ? ((clipboard->n_entries == 0) ? "L - ファイルをマーク (↑↓→←と併用)\nX - 削除 / [+R] 名前を変更\nY - コピー / [+R] エントリー作成\n" :
        "L - MARK files (use with ↑↓→←)\nX - 削除 / [+R] 名前の変更\nY - 貼り付け / [+R] エントリー作成\n") :
        ((GetWritePermissions() > PERM_BASE) ? "R+Y - 書き込み許可の再ロックs\n" : ""),
        (*curr_path) ? "" : (GetMountState()) ? "R+X - 切断\n" : "",
        (*curr_path) ? "" : (CheckSDMountState()) ? "R+B - SDカードを切断\n" : "R+B - SDカードの再接続\n",
        (*curr_path) ? "R+A - ディレクトリオプション\n" : "R+A - ドライブオプション\n",
        "R+L - スクリーンショット\n",
        "R+←→ - 前／次への切り替え\n",
        (clipboard->n_entries) ? "SELECT - クリップボードを削除\n" : "SELECT - クリップボードを復元\n", // only if clipboard is full
        "START - 再起動 / [+R] 電源を切る\nHOMEボタン ホームメニュー"); // generic end part
    DrawStringF(MAIN_SCREEN, instr_x, SCREEN_HEIGHT - 4 - GetDrawStringHeight(instr), COLOR_STD_FONT, COLOR_STD_BG, instr);
}

void DrawDirContents(DirStruct* contents, u32 cursor, u32* scroll) {
    const int str_width = (SCREEN_WIDTH_ALT-3) / FONT_WIDTH_EXT;
    const u32 stp_y = min(12, FONT_HEIGHT_EXT + 4);
    const u32 start_y = (MAIN_SCREEN == TOP_SCREEN) ? 0 : 12;
    const u32 pos_x = 0;
    const u32 lines = (SCREEN_HEIGHT-(start_y+2)+(stp_y-1)) / stp_y;
    u32 pos_y = start_y + 2;

    if (*scroll > cursor) *scroll = cursor;
    else if (*scroll + lines <= cursor) *scroll = cursor - lines + 1;
    if (*scroll + lines > contents->n_entries)
        *scroll = (contents->n_entries > lines) ? contents->n_entries - lines : 0;

    for (u32 i = 0; pos_y < SCREEN_HEIGHT; i++) {
        char tempstr[UTF_BUFFER_BYTESIZE(str_width)];
        u32 offset_i = *scroll + i;
        u32 color_font = COLOR_WHITE;
        if (offset_i < contents->n_entries) {
            DirEntry* curr_entry = &(contents->entry[offset_i]);
            char namestr[UTF_BUFFER_BYTESIZE(str_width - 10)];
            char bytestr[10 + 1];
            color_font = (cursor != offset_i) ? COLOR_ENTRY(curr_entry) : COLOR_STD_FONT;
            FormatBytes(bytestr, curr_entry->size);
            ResizeString(namestr, curr_entry->name, str_width - 10, str_width - 20, false);
            snprintf(tempstr, str_width * 4 + 1, "%s%10.10s", namestr,
                (curr_entry->type == T_DIR) ? "(dir)" : (curr_entry->type == T_DOTDOT) ? "(..)" : bytestr);
        } else snprintf(tempstr, str_width + 1, "%-*.*s", str_width, str_width, "");
        DrawStringF(ALT_SCREEN, pos_x, pos_y, color_font, COLOR_STD_BG, "%s", tempstr);
        pos_y += stp_y;
    }

    const u32 flist_height = (SCREEN_HEIGHT - start_y);
    const u32 bar_width = 2;
    if (contents->n_entries > lines) { // draw position bar at the right
        const u32 bar_height_min = 32;
        u32 bar_height = (lines * flist_height) / contents->n_entries;
        if (bar_height < bar_height_min) bar_height = bar_height_min;
        const u32 bar_pos = ((u64) *scroll * (flist_height - bar_height)) / (contents->n_entries - lines) + start_y;

        DrawRectangle(ALT_SCREEN, SCREEN_WIDTH_ALT - bar_width, start_y, bar_width, (bar_pos - start_y), COLOR_STD_BG);
        DrawRectangle(ALT_SCREEN, SCREEN_WIDTH_ALT - bar_width, bar_pos + bar_height, bar_width, SCREEN_HEIGHT - (bar_pos + bar_height), COLOR_STD_BG);
        DrawRectangle(ALT_SCREEN, SCREEN_WIDTH_ALT - bar_width, bar_pos, bar_width, bar_height, COLOR_SIDE_BAR);
    } else DrawRectangle(ALT_SCREEN, SCREEN_WIDTH_ALT - bar_width, start_y, bar_width, flist_height, COLOR_STD_BG);
}

u32 SdFormatMenu(const char* slabel) {
    static const u32 cluster_size_table[5] = { 0x0, 0x0, 0x4000, 0x8000, 0x10000 };
    static const char* option_emunand_size[7] = { "EmuNANDを作らない", "RedNAND 容量 (最小)", "GW EmuNAND 容量 (最大)",
        "マルチNAND 容量 (2x)", "マルチNAND 容量 (3x)", "マルチNAND 容量 (4x)", "ユーザー入力..." };
    static const char* option_cluster_size[4] = { "自動", "16KB クラスター", "32KB クラスター", "64KB クラスター" };
    u32 sysnand_min_size_sectors = GetNandMinSizeSectors(NAND_SYSNAND);
    u64 sysnand_min_size_mb = ((sysnand_min_size_sectors * 0x200) + 0xFFFFF) / 0x100000;
    u64 sysnand_multi_size_mb = (align(sysnand_min_size_sectors + 1, 0x2000) * 0x200) / 0x100000;
    u64 sysnand_size_mb = (((u64)GetNandSizeSectors(NAND_SYSNAND) * 0x200) + 0xFFFFF) / 0x100000;
    char label[DRV_LABEL_LEN + 4];
    u32 cluster_size = 0;
    u64 sdcard_size_mb = 0;
    u64 emunand_size_mb = (u64) -1;
    u32 user_select;

    // check actual SD card size
    sdcard_size_mb = GetSDCardSize() / 0x100000;
    if (!sdcard_size_mb) {
        ShowPrompt(false, "エラー: SDカードが検出されません。");
        return 1;
    }

    user_select = ShowSelectPrompt(7, option_emunand_size, "SDカードをフォーマット(%lluMB)?\nEmuNAND容量選択:", sdcard_size_mb);
    if (user_select && (user_select < 4)) {
        emunand_size_mb = (user_select == 2) ? sysnand_min_size_mb : (user_select == 3) ? sysnand_size_mb : 0;
    } else if ((user_select >= 4) && (user_select <= 6)) {
        u32 n = (user_select - 2);
        emunand_size_mb = n * sysnand_multi_size_mb;
    } else if (user_select == 7) do {
        emunand_size_mb = ShowNumberPrompt(sysnand_min_size_mb, "SDカードの容量%lluMB.\nEmuNAND容量入力 (MB) :", sdcard_size_mb);
        if (emunand_size_mb == (u64) -1) break;
    } while (emunand_size_mb > sdcard_size_mb);
    if (emunand_size_mb == (u64) -1) return 1;

    user_select = ShowSelectPrompt(4, option_cluster_size, "SDカードをフォーマットしますか (%lluMB)?\nCクラスター容量選択:", sdcard_size_mb);
    if (!user_select) return 1;
    else cluster_size = cluster_size_table[user_select];

    snprintf(label, DRV_LABEL_LEN + 4, "0:%s", (slabel && *slabel) ? slabel : "GM9SD");
    if (!ShowKeyboardOrPrompt(label + 2, 11 + 1, "SDカードをフォーマットしますか(%lluMB)?\nラベル入力:", sdcard_size_mb))
        return 1;

    if (!FormatSDCard(emunand_size_mb, cluster_size, label)) {
        ShowPrompt(false, "SDカードのフォーマット失敗!");
        return 1;
    }

    if (emunand_size_mb >= sysnand_min_size_mb) {
        u32 emunand_offset = 1;
        u32 n_emunands = 1;
        if (emunand_size_mb >= 2 * sysnand_size_mb) {
            static const char* option_emunand_type[4] = { "RedNANDタイプ (マルチ)", "RedNANDタイプ　(単体)", "GW EmuNANDタイプ", "設定しないでください" };
            user_select = ShowSelectPrompt(4, option_emunand_type, "セットアップするEmuNANDの種類を選択:");
            if (user_select > 3) return 0;
            emunand_offset = (user_select == 3) ? 0 : 1;
            if (user_select == 1) n_emunands = 4;
        } else if (emunand_size_mb >= sysnand_size_mb) {
            static const char* option_emunand_type[3] = { "RedNANDタイプ", "GW EmuNANDタイプ", "設定しないでください" };
            user_select = ShowSelectPrompt(3, option_emunand_type, "セットアップするEmuNANDの種類を選択:");
            if (user_select > 2) return 0;
            emunand_offset = (user_select == 2) ? 0 : 1; // 0 -> GW EmuNAND
        } else user_select = ShowPrompt(true, "SysNANDをRedNANDにクローンしますか") ? 1 : 0;
        if (!user_select) return 0;

        u8 ncsd[0x200];
        u32 flags = OVERRIDE_PERM;
        InitSDCardFS(); // this has to be initialized for EmuNAND to work
        for (u32 i = 0; i < n_emunands; i++) {
            if ((i * sysnand_multi_size_mb) + sysnand_min_size_mb > emunand_size_mb) break;
            SetEmuNandBase((i * sysnand_multi_size_mb * 0x100000 / 0x200) + emunand_offset);
            if ((ReadNandSectors(ncsd, 0, 1, 0xFF, NAND_SYSNAND) != 0) ||
                (WriteNandSectors(ncsd, 0, 1, 0xFF, NAND_EMUNAND) != 0) ||
                (!PathCopy("E:", "S:/nand_minsize.bin", &flags))) {
                ShowPrompt(false, "SysNANDからEmuNANDへのクローニングに失敗しました!");
                break;
            }
        }
        DeinitSDCardFS();
    }

    return 0;
}

u32 FileGraphicsViewer(const char* path) {
    const u32 max_size = SCREEN_SIZE(ALT_SCREEN);
    u64 filetype = IdentifyFileType(path);
    u16* bitmap = NULL;
    u8* input = (u8*)malloc(max_size);
    u32 w = 0;
    u32 h = 0;
    u32 ret = 1;

    if (!input)
        return ret;

    u32 input_size = FileGetData(path, input, max_size, 0);
    if (input_size && (input_size < max_size)) {
        if (filetype & GFX_PNG) {
            bitmap = PNG_Decompress(input, input_size, &w, &h);
            if (bitmap != NULL) ret = 0;
        }
    }

    if ((ret == 0) && w && h && (w <= SCREEN_WIDTH(ALT_SCREEN)) && (h <= SCREEN_HEIGHT)) {
        ClearScreenF(true, true, COLOR_STD_BG);
        DrawBitmap(ALT_SCREEN, -1, -1, w, h, bitmap);
        ShowString("<A>ボタンを押して続ける");
        while(!(InputWait(0) & (BUTTON_A | BUTTON_B)));
        ClearScreenF(true, true, COLOR_STD_BG);
    } else ret = 1;

    free(bitmap);
    free(input);
    return ret;
}

u32 FileHexViewer(const char* path) {
    const u32 max_data = (SCREEN_HEIGHT / FONT_HEIGHT_EXT) * 16 * ((FONT_WIDTH_EXT > 4) ? 1 : 2);
    static u32 mode = 0;
    u8* data = NULL;
    u8* bottom_cpy = (u8*) malloc(SCREEN_SIZE_BOT); // a copy of the bottom screen framebuffer
    u32 fsize = FileGetSize(path);

    bool dual_screen = 0;
    int x_off = 0, x_hex = 0, x_ascii = 0;
    u32 vpad = 0, hlpad = 0, hrpad = 0;
    u32 rows = 0, cols = 0;
    u32 total_shown = 0;
    u32 total_data = 0;

    u32 last_mode = 0xFF;
    u32 last_offset = (u32) -1;
    u32 offset = 0;

    u8  found_data[64 + 1] = { 0 };
    u32 found_offset = (u32) -1;
    u32 found_size = 0;

    static const u32 edit_bsize = 0x4000; // should be multiple of 0x200 * 2
    bool edit_mode = false;
    u8* buffer = (u8*) malloc(edit_bsize);
    u8* buffer_cpy = (u8*) malloc(edit_bsize);
    u32 edit_start = 0;
    int cursor = 0;

    if (!bottom_cpy || !buffer || !buffer_cpy) {
        if (bottom_cpy) free(bottom_cpy);
        if (buffer) free(buffer);
        if (buffer_cpy) free(buffer_cpy);
        return 1;
    }

    static bool show_instr = true;
    static const char* instr = "Hexeditorコントロール:\n \n↑↓→←(+R) - スクロール\nR+Y - ビュー切り替え\nX - 検索 / へ...\nA - 編集モードに入る\nA+↑↓→← - 編集値\nB - 退出\n";
    if (show_instr) { // show one time instructions
        ShowPrompt(false, instr);
        show_instr = false;
    }

    if (MAIN_SCREEN != TOP_SCREEN) ShowString(instr);
    memcpy(bottom_cpy, BOT_SCREEN, SCREEN_SIZE_BOT);

    data = buffer;
    while (true) {
        if (mode != last_mode) {
            if (FONT_WIDTH_EXT <= 5) {
                mode = 0;
                vpad = 1;
                hlpad = hrpad = 2;
                cols = 16;
                x_off = (SCREEN_WIDTH_TOP - SCREEN_WIDTH_BOT) / 2;
                x_ascii = SCREEN_WIDTH_TOP - x_off - (FONT_WIDTH_EXT * cols);
                x_hex = ((SCREEN_WIDTH_TOP - ((hlpad + (2*FONT_WIDTH_EXT) + hrpad) * cols)) / 2) -
                    (((cols - 8) / 2) * FONT_WIDTH_EXT);
                dual_screen = true;
            } else if (FONT_WIDTH_EXT <= 6) {
                if (mode == 1) {
                    vpad = 0;
                    hlpad = hrpad = 1;
                    cols = 16;
                    x_off = 0;
                    x_ascii = SCREEN_WIDTH_TOP - (FONT_WIDTH_EXT * cols);
                    x_hex = x_off + (8*FONT_WIDTH_EXT) + 16;
                    dual_screen = false;
                } else {
                    mode = 0;
                    vpad = 0;
                    hlpad = hrpad = 3;
                    cols = 8;
                    x_off = 30 + (SCREEN_WIDTH_TOP - SCREEN_WIDTH_BOT) / 2;
                    x_ascii = SCREEN_WIDTH_TOP - x_off - (FONT_WIDTH_EXT * cols);
                    x_hex = (SCREEN_WIDTH_TOP - ((hlpad + (2*FONT_WIDTH_EXT) + hrpad) * cols)) / 2;
                    dual_screen = true;
                }
            } else switch (mode) { // display mode
                case 1:
                    vpad = hlpad = hrpad = 1;
                    cols = 12;
                    x_off = 0;
                    x_ascii = SCREEN_WIDTH_TOP - (FONT_WIDTH_EXT * cols);
                    x_hex = ((SCREEN_WIDTH_TOP - ((hlpad + (2*FONT_WIDTH_EXT) + hrpad) * cols) -
                        ((cols - 8) * FONT_WIDTH_EXT)) / 2);
                    dual_screen = false;
                    break;
                case 2:
                    vpad = 1;
                    hlpad = 0;
                    hrpad = 1 + 8 - FONT_WIDTH_EXT;
                    cols = 16;
                    x_off = -1;
                    x_ascii = SCREEN_WIDTH_TOP - (FONT_WIDTH_EXT * cols);
                    x_hex = 0;
                    dual_screen = false;
                    break;
                case 3:
                    vpad = hlpad = hrpad = 1;
                    cols = 16;
                    x_off = ((SCREEN_WIDTH_TOP - ((hlpad + (2*FONT_WIDTH_EXT) + hrpad) * cols)
                        - 12 - (8*FONT_WIDTH_EXT)) / 2);
                    x_ascii = -1;
                    x_hex = x_off + (8*FONT_WIDTH_EXT) + 12;
                    dual_screen = false;
                    break;
                default:
                    mode = 0;
                    vpad = hlpad = hrpad = 2;
                    cols = 8;
                    x_off = (SCREEN_WIDTH_TOP - SCREEN_WIDTH_BOT) / 2;
                    x_ascii = SCREEN_WIDTH_TOP - x_off - (FONT_WIDTH_EXT * cols);
                    x_hex = (SCREEN_WIDTH_TOP - ((hlpad + (2*FONT_WIDTH_EXT) + hrpad) * cols)) / 2;
                    dual_screen = true;
                    break;
            }
            rows = (dual_screen ? 2 : 1) * SCREEN_HEIGHT / (FONT_HEIGHT_EXT + (2*vpad));
            total_shown = rows * cols;
            last_mode = mode;
            ClearScreen(TOP_SCREEN, COLOR_STD_BG);
            if (dual_screen) ClearScreen(BOT_SCREEN, COLOR_STD_BG);
            else memcpy(BOT_SCREEN, bottom_cpy, SCREEN_SIZE_BOT);
        }
        // fix offset (if required)
        if (offset % cols) offset -= (offset % cols); // fix offset (align to cols)
        if (offset + total_shown - cols > fsize) // if offset too big
            offset = (total_shown > fsize) ? 0 : (fsize + cols - total_shown - (fsize % cols));
        // get data, using max data size (if new offset)
        if (offset != last_offset) {
            if (!edit_mode) {
                total_data = FileGetData(path, data, max_data, offset);
            } else { // edit mode - read from memory
                if ((offset < edit_start) || (offset + max_data > edit_start + edit_bsize))
                    offset = last_offset; // we don't expect this to happen
                total_data = (fsize - offset >= max_data) ? max_data : fsize - offset;
                data = buffer + (offset - edit_start);
            }
            last_offset = offset;
        }

        // display data on screen
        for (u32 row = 0; row < rows; row++) {
            char ascii[16 + 1] = { 0 };
            u32 y = row * (FONT_HEIGHT_EXT + (2*vpad)) + vpad;
            u32 curr_pos = row * cols;
            u32 cutoff = (curr_pos >= total_data) ? 0 : (total_data >= curr_pos + cols) ? cols : total_data - curr_pos;
            u16* screen = TOP_SCREEN;
            u32 x0 = 0;

            // marked offsets handling
            s32 marked0 = 0, marked1 = 0;
            if ((found_size > 0) &&
                (found_offset + found_size > offset + curr_pos) &&
                (found_offset < offset + curr_pos + cols)) {
                marked0 = (s32) found_offset - (offset + curr_pos);
                marked1 = marked0 + found_size;
                if (marked0 < 0) marked0 = 0;
                if (marked1 > (s32) cols) marked1 = (s32) cols;
            }

            // switch to bottom screen
            if (y >= SCREEN_HEIGHT) {
                y -= SCREEN_HEIGHT;
                screen = BOT_SCREEN;
                x0 = 40;
            }

            memcpy(ascii, data + curr_pos, cols);
            for (u32 col = 0; col < cols; col++)
                if ((col >= cutoff) || (ascii[col] == 0x00)) ascii[col] = ' ';

            // draw offset / ASCII representation
            if (x_off >= 0) DrawStringF(screen, x_off - x0, y, cutoff ? COLOR_HVOFFS : COLOR_HVOFFSI,
                COLOR_STD_BG, "%08X", (unsigned int) offset + curr_pos);
            if (x_ascii >= 0) {
                for (u32 i = 0; i < cols; i++)
                    DrawCharacter(screen, ascii[i], x_ascii - x0 + (FONT_WIDTH_EXT * i), y, COLOR_HVASCII, COLOR_STD_BG);
                for (u32 i = (u32) marked0; i < (u32) marked1; i++)
                    DrawCharacter(screen, ascii[i % cols], x_ascii - x0 + (FONT_WIDTH_EXT * i), y, COLOR_MARKED, COLOR_STD_BG);
                if (edit_mode && ((u32) cursor / cols == row)) DrawCharacter(screen, ascii[cursor % cols],
                    x_ascii - x0 + FONT_WIDTH_EXT * (cursor % cols), y, COLOR_RED, COLOR_STD_BG);
            }

            // draw HEX values
            for (u32 col = 0; (col < cols) && (x_hex >= 0); col++) {
                u32 x = (x_hex + hlpad) + (((2*FONT_WIDTH_EXT) + hrpad + hlpad) * col) - x0;
                u32 hex_color = (edit_mode && ((u32) cursor == curr_pos + col)) ? COLOR_RED :
                    (((s32) col >= marked0) && ((s32) col < marked1)) ? COLOR_MARKED : COLOR_HVHEX(col);
                if (col < cutoff)
                    DrawStringF(screen, x, y, hex_color, COLOR_STD_BG, "%02X", (unsigned int) data[curr_pos + col]);
                else DrawStringF(screen, x, y, hex_color, COLOR_STD_BG, "  ");
            }
        }

        // handle user input
        u32 pad_state = InputWait(0);
        if (!edit_mode) { // standard viewer mode
            u32 step_ud = (pad_state & BUTTON_R1) ? (0x1000  - (0x1000  % cols)) : cols;
            u32 step_lr = (pad_state & BUTTON_R1) ? (0x10000 - (0x10000 % cols)) : total_shown;
            if (pad_state & BUTTON_DOWN) offset += step_ud;
            else if (pad_state & BUTTON_RIGHT) offset += step_lr;
            else if (pad_state & BUTTON_UP) offset = (offset > step_ud) ? offset - step_ud : 0;
            else if (pad_state & BUTTON_LEFT) offset = (offset > step_lr) ? offset - step_lr : 0;
            else if ((pad_state & BUTTON_R1) && (pad_state & BUTTON_Y)) mode++;
            else if ((pad_state & BUTTON_A) && total_data) edit_mode = true;
            else if (pad_state & (BUTTON_B|BUTTON_START)) break;
            else if (found_size && (pad_state & BUTTON_R1) && (pad_state & BUTTON_X)) {
                found_offset = FileFindData(path, found_data, found_size, found_offset + 1);
                if (found_offset == (u32) -1) {
                    ShowPrompt(false, "見つかりませんでした!");
                    found_size = 0;
                } else offset = found_offset;
                if (MAIN_SCREEN == TOP_SCREEN) ClearScreen(TOP_SCREEN, COLOR_STD_BG);
                else if (dual_screen) ClearScreen(BOT_SCREEN, COLOR_STD_BG);
                else memcpy(BOT_SCREEN, bottom_cpy, SCREEN_SIZE_BOT);
            } else if (pad_state & BUTTON_X) {
                static const char* optionstr[3] = { "オフセットへ", "文字列を検索する", "データ検索" };
                u32 user_select = ShowSelectPrompt(3, optionstr, "現在のオフセッ: %08X\nアクションを選択:",
                    (unsigned int) offset);
                if (user_select == 1) { // -> goto offset
                    u64 new_offset = ShowHexPrompt(offset, 8, "現在のオフセット: %08X\n新しいオフセットを以下に入力します。.",
                        (unsigned int) offset);
                    if (new_offset != (u64) -1) offset = new_offset;
                } else if (user_select == 2) {
                    if (!found_size) *found_data = 0;
                    if (ShowKeyboardOrPrompt((char*) found_data, 64 + 1, "検索文字列を入力してください。\n(R+X を押すすると、検索を繰り返すことができます。)")) {
                        found_size = strnlen((char*) found_data, 64);
                        found_offset = FileFindData(path, found_data, found_size, offset);
                        if (found_offset == (u32) -1) {
                            ShowPrompt(false, "見つかりませんでした!");
                            found_size = 0;
                        } else offset = found_offset;
                    }
                } else if (user_select == 3) {
                    u32 size = found_size;
                    if (ShowDataPrompt(found_data, &size, "検索データを入力してください。\n(R+X を押すすると、検索を繰り返すことができます。)")) {
                        found_size = size;
                        found_offset = FileFindData(path, found_data, size, offset);
                        if (found_offset == (u32) -1) {
                            ShowPrompt(false, "見つかりませんでした!");
                            found_size = 0;
                        } else offset = found_offset;
                    }
                }
                if (MAIN_SCREEN == TOP_SCREEN) ClearScreen(TOP_SCREEN, COLOR_STD_BG);
                else if (dual_screen) ClearScreen(BOT_SCREEN, COLOR_STD_BG);
                else memcpy(BOT_SCREEN, bottom_cpy, SCREEN_SIZE_BOT);
            }
            if (edit_mode && CheckWritePermissions(path)) { // setup edit mode
                found_size = 0;
                found_offset = (u32) -1;
                cursor = 0;
                edit_start = ((offset - (offset % 0x200) <= (edit_bsize / 2)) || (fsize < edit_bsize)) ? 0 :
                    offset - (offset % 0x200) - (edit_bsize / 2);
                FileGetData(path, buffer, edit_bsize, edit_start);
                memcpy(buffer_cpy, buffer, edit_bsize);
                data = buffer + (offset - edit_start);
            } else edit_mode = false;
        } else { // editor mode
            if (pad_state & (BUTTON_B|BUTTON_START)) {
                edit_mode = false;
                // check for user edits
                u32 diffs = 0;
                for (u32 i = 0; i < edit_bsize; i++) if (buffer[i] != buffer_cpy[i]) diffs++;
                if (diffs && ShowPrompt(true, "あなたが編集したのは %i です。\n変更をファイルに書き込みますか", diffs))
                    if (!FileSetData(path, buffer, min(edit_bsize, (fsize - edit_start)), edit_start, false))
                        ShowPrompt(false, "ファイルへの書き込みに失敗しました!");
                data = buffer;
                last_offset = (u32) -1; // force reload from file
            } else if (pad_state & BUTTON_A) {
                if (pad_state & BUTTON_DOWN) data[cursor]--;
                else if (pad_state & BUTTON_UP) data[cursor]++;
                else if (pad_state & BUTTON_RIGHT) data[cursor] += 0x10;
                else if (pad_state & BUTTON_LEFT) data[cursor] -= 0x10;
            } else {
                if (pad_state & BUTTON_DOWN) cursor += cols;
                else if (pad_state & BUTTON_UP) cursor -= cols;
                else if (pad_state & BUTTON_RIGHT) cursor++;
                else if (pad_state & BUTTON_LEFT) cursor--;
                // fix cursor position
                if (cursor < 0) {
                    if (offset >= cols) {
                        offset -= cols;
                        cursor += cols;
                    } else cursor = 0;
                } else if (((u32) cursor >= total_data) && (total_data < total_shown)) {
                    cursor = total_data - 1;
                } else if ((u32) cursor >= total_shown) {
                    if (offset + total_shown == fsize) {
                        cursor = total_shown - 1;
                    } else {
                        offset += cols;
                        cursor = (offset + cursor >= fsize) ? fsize - offset - 1 : cursor - cols;
                    }
                }
            }
        }
    }

    ClearScreen(TOP_SCREEN, COLOR_STD_BG);
    if (MAIN_SCREEN == TOP_SCREEN) memcpy(BOT_SCREEN, bottom_cpy, SCREEN_SIZE_BOT);
    else ClearScreen(BOT_SCREEN, COLOR_STD_BG);

    free(bottom_cpy);
    free(buffer);
    free(buffer_cpy);
    return 0;
}

u32 ShaCalculator(const char* path, bool sha1) {
    const u8 hashlen = sha1 ? 20 : 32;
    u32 drvtype = DriveType(path);
    char pathstr[UTF_BUFFER_BYTESIZE(32)];
    u8 hash[32];
    TruncateString(pathstr, path, 32, 8);
    if (!FileGetSha(path, hash, 0, 0, sha1)) {
        ShowPrompt(false, "SHAを計算する-%s: 失敗!", sha1 ? "1" : "256");
        return 1;
    } else {
        static char pathstr_prev[32 + 1] = { 0 };
        static u8 hash_prev[32] = { 0 };
        char sha_path[256];
        u8 sha_file[32];

        snprintf(sha_path, 256, "%s.sha%c", path, sha1 ? '1' : '\0');
        bool have_sha = (FileGetData(sha_path, sha_file, hashlen, 0) == hashlen);
        bool match_sha = have_sha && (memcmp(hash, sha_file, hashlen) == 0);
        bool match_prev = (memcmp(hash, hash_prev, hashlen) == 0);
        bool write_sha = (!have_sha || !match_sha) && (drvtype & DRV_SDCARD); // writing only on SD
        char hash_str[32+1+32+1];
        if (sha1)
            snprintf(hash_str, 20+1+20+1, "%016llX%04X\n%016llX%04X", getbe64(hash + 0), getbe16(hash + 8),
            getbe64(hash + 10), getbe16(hash + 18));
        else
            snprintf(hash_str, 32+1+32+1, "%016llX%016llX\n%016llX%016llX", getbe64(hash + 0), getbe64(hash + 8),
            getbe64(hash + 16), getbe64(hash + 24));
        if (ShowPrompt(write_sha, "%s\n%s%s%s%s%s%c \nWrite .SHA%s file?",
            pathstr, hash_str,
            (have_sha) ? "\nSHA検証: " : "",
            (have_sha) ? ((match_sha) ? "合格!" : "失敗!") : "",
            (match_prev) ? "\n \n前のファイルと同じです。:\n" : "",
            (match_prev) ? pathstr_prev : "",
            (write_sha) ? '\n' : '\0',
            (sha1) ? "1" : "") && write_sha) {
            FileSetData(sha_path, hash, hashlen, 0, true);
        }

        strncpy(pathstr_prev, pathstr, 32 + 1);
        memcpy(hash_prev, hash, hashlen);
    }

    return 0;
}

u32 CmacCalculator(const char* path) {
    char pathstr[UTF_BUFFER_BYTESIZE(32)];
    TruncateString(pathstr, path, 32, 8);
    if (IdentifyFileType(path) != GAME_CMD) {
        u8 cmac[16] __attribute__((aligned(4)));
        if (CalculateFileCmac(path, cmac) != 0) {
            ShowPrompt(false, "CMACの算出: 失敗!");
            return 1;
        } else {
            u8 cmac_file[16];
            bool identical = ((ReadFileCmac(path, cmac_file) == 0) && (memcmp(cmac, cmac_file, 16) == 0));
            if (ShowPrompt(!identical, "%s\n%016llX%016llX\n%s%s%s",
                pathstr, getbe64(cmac + 0), getbe64(cmac + 8),
                "CMAC検証: ", (identical) ? "合格" : "失敗!",
                (!identical) ? "\n \nファイル内のCMACを修正しますか?" : "") &&
                !identical && (WriteFileCmac(path, cmac, true) != 0)) {
                ShowPrompt(false, "CMACの修正: 失敗!");
            }
        }
    } else { // special handling for CMD files
        bool correct = (CheckCmdCmac(path) == 0);
        if (ShowPrompt(!correct, "%s\nXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\n%s%s%s",
            pathstr, "CMAC検証: ", (correct) ? "合格!" : "失敗!",
            (!correct) ? "\n \nCMACをファイルに修正しますか？" : "") &&
            !correct && (FixCmdCmac(path, true) != 0)) {
            ShowPrompt(false, "CMACの修正: 失敗!");
        }
    }


    return 0;
}

u32 StandardCopy(u32* cursor, u32* scroll) {
    DirEntry* curr_entry = &(current_dir->entry[*cursor]);
    u32 n_marked = 0;
    if (curr_entry->marked) {
        for (u32 i = 0; i < current_dir->n_entries; i++)
            if (current_dir->entry[i].marked) n_marked++;
    }

    u32 flags = BUILD_PATH;
    if ((n_marked > 1) && ShowPrompt(true, "選択されたすべての%luをコピーしますか?", n_marked)) {
        u32 n_success = 0;
        for (u32 i = 0; i < current_dir->n_entries; i++) {
            const char* path = current_dir->entry[i].path;
            if (!current_dir->entry[i].marked)
                continue;
            flags |= ASK_ALL;
            DrawDirContents(current_dir, (*cursor = i), scroll);
            if (PathCopy(OUTPUT_PATH, path, &flags)) n_success++;
            else { // on failure: show error, break
                char currstr[UTF_BUFFER_BYTESIZE(32)];
                TruncateString(currstr, path, 32, 12);
                ShowPrompt(false, "%s\nアイテムのコピーに失敗しました", currstr);
                break;
            }
            current_dir->entry[i].marked = false;
        }
        if (n_success) ShowPrompt(false, "%lu コピーされた項目 %s", n_success, OUTPUT_PATH);
    } else {
        char pathstr[UTF_BUFFER_BYTESIZE(32)];
        TruncateString(pathstr, curr_entry->path, 32, 8);
        if (!PathCopy(OUTPUT_PATH, curr_entry->path, &flags))
            ShowPrompt(false, "%s\nアイテムのコピーに失敗しました", pathstr);
        else ShowPrompt(false, "%s\nコピーされました %s", pathstr, OUTPUT_PATH);
    }

    return 0;
}

u32 CartRawDump(void) {
    CartData* cdata = (CartData*) malloc(sizeof(CartData));
    char dest[256];
    char cname[24];
    char bytestr[32];
    u64 dsize = 0;

    if (!cdata ||(InitCartRead(cdata) != 0) || (GetCartName(cname, cdata) != 0)) {
        ShowPrompt(false, "カートの起動に失敗しました!");
        free(cdata);
        return 1;
    }

    // input dump size
    dsize = cdata->cart_size;
    FormatBytes(bytestr, dsize);
    dsize = ShowHexPrompt(dsize, 8, "カート: %s\n容量を検出しました。: %s\n \nダンプサイズを以下に入力します。", cname, bytestr);
    if (!dsize || (dsize == (u64) -1)) {
        free(cdata);
        return 1;
    }

    // for NDS carts: ask for secure area encryption
    if (cdata->cart_type & CART_NTR)
        SetSecureAreaEncryption(
            !ShowPrompt(true, "カート: %s\nNDSカートを検出\nセキュアエリアを復号化しますか?", cname));

    // destination path
    snprintf(dest, 256, "%s/%s_%08llX.%s",
        OUTPUT_PATH, cname, dsize, (cdata->cart_type & CART_CTR) ? "3ds" : "nds");

    // buffer allocation
    u8* buf = (u8*) malloc(STD_BUFFER_SIZE);
    if (!buf) { // this will not happen
        free(cdata);
        return 1;
    }

    // actual cart dump
    u32 ret = 0;
    PathDelete(dest);
    ShowProgress(0, 0, cname);
    for (u64 p = 0; p < dsize; p += STD_BUFFER_SIZE) {
        u64 len = min((dsize - p), STD_BUFFER_SIZE);
        if ((ReadCartBytes(buf, p, len, cdata, false) != 0) ||
            (fvx_qwrite(dest, buf, p, len, NULL) != FR_OK) ||
            !ShowProgress(p, dsize, cname)) {
            PathDelete(dest);
            ret = 1;
            break;
        }
    }

    if (ret) ShowPrompt(false, "%s\nカートダンプに失敗", cname);
    else ShowPrompt(false, "%s\nダンプ %s", cname, OUTPUT_PATH);
    
    free(buf);
    free(cdata);
    return ret;
}

u32 DirFileAttrMenu(const char* path, const char *name) {
    bool drv = (path[2] == '\0');
    bool vrt = (!drv); // will be checked below
    char namestr[128], datestr[32], attrstr[128], sizestr[192];
    FILINFO fno;
    u8 new_attrib;

    // create mutiline name string
    MultiLineString(namestr, name, 31, 4);

    // preparations: create file info, date string
    if (!drv) {
        if (fvx_stat(path, &fno) != FR_OK) return 1;
        vrt = (fno.fattrib & AM_VRT);
        new_attrib = fno.fattrib;
        snprintf(datestr, 32, "%s: %04d-%02d-%02d %02d:%02d:%02d\n",
            (fno.fattrib & AM_DIR) ? "作成済み" : "編集された",
            1980 + ((fno.fdate >> 9) & 0x7F), (fno.fdate >> 5) & 0xF, fno.fdate & 0x1F,
            (fno.ftime >> 11) & 0x1F, (fno.ftime >> 5) & 0x3F, (fno.ftime & 0x1F) << 1);
    } else {
        *datestr = '\0';
        *attrstr = '\0';
        new_attrib = 0;
    }

    // create size string
    if (drv || (fno.fattrib & AM_DIR)) { // for dirs and drives
        char bytestr[32];
        u64 tsize = 0;
        u32 tdirs = 0;
        u32 tfiles = 0;

        // this may take a while...
        ShowString("%s　を解析しています。...", drv ? "ドライブ" : "ディレクトリ");
        if (!DirInfo(path, &tsize, &tdirs, &tfiles))
            return 1;
        FormatBytes(bytestr, tsize);

        if (drv) { // drive specific
            char freestr[32], drvsstr[32], usedstr[32];
            FormatBytes(freestr, GetFreeSpace(path));
            FormatBytes(drvsstr, GetTotalSpace(path));
            FormatBytes(usedstr, GetTotalSpace(path) - GetFreeSpace(path));
            snprintf(sizestr, 192, "%lu ファイル & %lu サブディレクトリ\n%s 合計サイズ\n \n空き容量: %s\n使用されています: %s\n全空き容量: %s",
                tfiles, tdirs, bytestr, freestr, usedstr, drvsstr);
        } else { // dir specific
            snprintf(sizestr, 192, "%lu ファイル & %lu サブディレクトリ\n%s 合計サイズ",
                tfiles, tdirs, bytestr);
        }
    } else { // for files
        char bytestr[32];
        FormatBytes(bytestr, fno.fsize);
        snprintf(sizestr, 64, "容量: %s", bytestr);
    }

    while(true) {
        if (!drv) {
            snprintf(attrstr, 128,
                " \n"
                "[%c] %スレッドオンリー  [%c] %s隠しファイル\n"
                "[%c] %sシステム     [%c] %s圧縮ファイル\n"
                "[%c] %s仮想\n"
                "%s",
                (new_attrib & AM_RDO) ? 'X' : ' ', vrt ? "" : "↑",
                (new_attrib & AM_HID) ? 'X' : ' ', vrt ? "" : "↓",
                (new_attrib & AM_SYS) ? 'X' : ' ', vrt ? "" : "→",
                (new_attrib & AM_ARC) ? 'X' : ' ', vrt ? "" : "←",
                vrt ? 'X' : ' ', vrt ? "" : "  ",
                vrt ? "" : " \n(↑↓→← 属性を変更する)\n"
            );
        }

        ShowString(
            "%s\n \n"   // name
            "%s"        // date (not for drives)
            "%s\n"      // size
            "%s \n"     // attr (not for drives)
            "%s\n",     // options
            namestr, datestr, sizestr, attrstr,
            (drv || vrt || (new_attrib == fno.fattrib)) ? "(<A>ボタンを押してで続ける)" : "(<A>適用する, <B>キャンセル)"
        );

        while(true) {
            u32 pad_state = InputWait(0);

            if (pad_state & (BUTTON_A | BUTTON_B)) {
                if (!drv && !vrt) {
                    const u8 mask = (AM_RDO | AM_HID | AM_SYS | AM_ARC);
                    bool apply = (new_attrib != fno.fattrib) && (pad_state & BUTTON_A);
                    if (apply && !PathAttr(path, new_attrib & mask, mask)) {
                        ShowPrompt(false, "%s\n属性の設定に失敗しました", namestr);
                    }
                }
                ClearScreenF(true, false, COLOR_STD_BG);
                return 0;
            }

            if (!drv && !vrt && (pad_state & BUTTON_ARROW)) {
                switch (pad_state & BUTTON_ARROW) {
                    case BUTTON_UP:
                        new_attrib ^= AM_RDO;
                        break;
                    case BUTTON_DOWN:
                        new_attrib ^= AM_HID;
                        break;
                    case BUTTON_RIGHT:
                        new_attrib ^= AM_SYS;
                        break;
                    case BUTTON_LEFT:
                        new_attrib ^= AM_ARC;
                        break;
                }
                break;
            }
        }
    }
}

u32 FileHandlerMenu(char* current_path, u32* cursor, u32* scroll, PaneData** pane) {
    const char* file_path = (&(current_dir->entry[*cursor]))->path;
    const char* file_name = (&(current_dir->entry[*cursor]))->name;
    const char* optionstr[16];

    // check for file lock
    if (!FileUnlock(file_path)) return 1;

    u64 filetype = IdentifyFileType(file_path);
    u32 drvtype = DriveType(file_path);
    u64 tid = GetGameFileTitleId(file_path);

    bool in_output_path = (strncasecmp(current_path, OUTPUT_PATH, 256) == 0);

    // don't handle TMDs inside the game drive, won't work properly anyways
    if ((filetype & GAME_TMD) && (drvtype & DRV_GAME)) filetype &= ~GAME_TMD;

    // special stuff, only available for known filetypes (see int special below)
    bool mountable = (FTYPE_MOUNTABLE(filetype) && !(drvtype & DRV_IMAGE) &&
        !((drvtype & (DRV_SYSNAND|DRV_EMUNAND)) && (drvtype & DRV_VIRTUAL) && (filetype & IMG_FAT)));
    bool verificable = (FTYPE_VERIFICABLE(filetype));
    bool decryptable = (FTYPE_DECRYPTABLE(filetype));
    bool encryptable = (FTYPE_ENCRYPTABLE(filetype));
    bool cryptable_inplace = ((encryptable||decryptable) && !in_output_path && (*current_path == '0'));
    bool cia_buildable = (FTYPE_CIABUILD(filetype));
    bool cia_buildable_legit = (FTYPE_CIABUILD_L(filetype));
    bool cia_installable = (FTYPE_CIAINSTALL(filetype)) && !(drvtype & DRV_CTRNAND) &&
        !(drvtype & DRV_TWLNAND) && !(drvtype & DRV_ALIAS) && !(drvtype & DRV_IMAGE);
    bool tik_installable = (FTYPE_TIKINSTALL(filetype)) && !(drvtype & DRV_IMAGE);
    bool tik_dumpable = (FTYPE_TIKDUMP(filetype));
    bool cif_installable = (FTYPE_CIFINSTALL(filetype)) && !(drvtype & DRV_IMAGE);
    bool uninstallable = (FTYPE_UNINSTALL(filetype));
    bool cxi_dumpable = (FTYPE_CXIDUMP(filetype));
    bool tik_buildable = (FTYPE_TIKBUILD(filetype)) && !in_output_path;
    bool key_buildable = (FTYPE_KEYBUILD(filetype)) && !in_output_path &&
        !((drvtype & DRV_VIRTUAL) && (drvtype & DRV_SYSNAND));
    bool titleinfo = (FTYPE_TITLEINFO(filetype));
    bool renamable = (FTYPE_RENAMABLE(filetype)) && !(drvtype & DRV_VIRTUAL) && !(drvtype & DRV_ALIAS) &&
        !(drvtype & DRV_CTRNAND) && !(drvtype & DRV_TWLNAND) && !(drvtype & DRV_IMAGE);
    bool trimable = (FTYPE_TRIMABLE(filetype)) && !(drvtype & DRV_VIRTUAL) && !(drvtype & DRV_ALIAS) &&
        !(drvtype & DRV_CTRNAND) && !(drvtype & DRV_TWLNAND) && !(drvtype & DRV_IMAGE);
    bool transferable = (FTYPE_TRANSFERABLE(filetype) && IS_UNLOCKED && (drvtype & DRV_FAT));
    bool hsinjectable = (FTYPE_HASCODE(filetype));
    bool extrcodeable = (FTYPE_HASCODE(filetype));
    bool restorable = (FTYPE_RESTORABLE(filetype) && IS_UNLOCKED && !(drvtype & DRV_SYSNAND));
    bool ebackupable = (FTYPE_EBACKUP(filetype));
    bool ncsdfixable = (FTYPE_NCSDFIXABLE(filetype));
    bool xorpadable = (FTYPE_XORPAD(filetype));
    bool keyinitable = (FTYPE_KEYINIT(filetype)) && !((drvtype & DRV_VIRTUAL) && (drvtype & DRV_SYSNAND));
    bool keyinstallable = (FTYPE_KEYINSTALL(filetype)) && !((drvtype & DRV_VIRTUAL) && (drvtype & DRV_SYSNAND));
    bool scriptable = (FTYPE_SCRIPT(filetype));
    bool fontable = (FTYPE_FONT(filetype));
    bool viewable = (FTYPE_GFX(filetype));
    bool setable = (FTYPE_SETABLE(filetype));
    bool bootable = (FTYPE_BOOTABLE(filetype));
    bool installable = (FTYPE_INSTALLABLE(filetype));
    bool agbexportable = (FTYPE_AGBSAVE(filetype) && (drvtype & DRV_VIRTUAL) && (drvtype & DRV_SYSNAND));
    bool agbimportable = (FTYPE_AGBSAVE(filetype) && (drvtype & DRV_VIRTUAL) && (drvtype & DRV_SYSNAND));

    char cxi_path[256] = { 0 }; // special options for TMD
    if ((filetype & GAME_TMD) &&
        (GetTmdContentPath(cxi_path, file_path) == 0) &&
        (PathExist(cxi_path))) {
        u64 filetype_cxi = IdentifyFileType(cxi_path);
        mountable = (FTYPE_MOUNTABLE(filetype_cxi) && !(drvtype & DRV_IMAGE));
        extrcodeable = (FTYPE_HASCODE(filetype_cxi));
    }

    bool special_opt =
        mountable || verificable || decryptable || encryptable || cia_buildable || cia_buildable_legit ||
        cxi_dumpable || tik_buildable || key_buildable || titleinfo || renamable || trimable || transferable ||
        hsinjectable || restorable || xorpadable || ebackupable || ncsdfixable || extrcodeable || keyinitable ||
        keyinstallable || bootable || scriptable || fontable || viewable || installable || agbexportable ||
        agbimportable || cia_installable || tik_installable || tik_dumpable || cif_installable;

    char pathstr[UTF_BUFFER_BYTESIZE(32)];
    TruncateString(pathstr, file_path, 32, 8);

    char tidstr[32] = { 0 };
    if (tid) snprintf(tidstr, 32, "\ntid: <%016llX>", tid);

    u32 n_marked = 0;
    if ((&(current_dir->entry[*cursor]))->marked) {
        for (u32 i = 0; i < current_dir->n_entries; i++)
            if (current_dir->entry[i].marked) n_marked++;
    }

    // main menu processing
    int n_opt = 0;
    int special = (special_opt) ? ++n_opt : -1;
    int hexviewer = ++n_opt;
    int textviewer = (filetype & TXT_GENERIC) ? ++n_opt : -1;
    int calcsha256 = ++n_opt;
    int calcsha1 = ++n_opt;
    int calccmac = (CheckCmacPath(file_path) == 0) ? ++n_opt : -1;
    int fileinfo = ++n_opt;
    int copystd = (!in_output_path) ? ++n_opt : -1;
    int inject = ((clipboard->n_entries == 1) &&
        (clipboard->entry[0].type == T_FILE) &&
        (strncmp(clipboard->entry[0].path, file_path, 256) != 0)) ?
        (int) ++n_opt : -1;
    int searchdrv = (DriveType(current_path) & DRV_SEARCH) ? ++n_opt : -1;
    int titleman = -1;
    if (DriveType(current_path) & DRV_TITLEMAN) {
        // special case: title manager (disable almost everything)
        hexviewer = textviewer = calcsha256 = calcsha1 = calccmac = fileinfo = copystd = inject = searchdrv = -1;
        special = 1;
        titleman = 2;
        n_opt = 2;
    }
    if (special > 0) optionstr[special-1] =
        (filetype & IMG_NAND  ) ? "NANDイメージオプション..." :
        (filetype & IMG_FAT   ) ? (transferable) ? "CTRNANDオプション..." : "FATイメージとして接続する" :
        (filetype & GAME_CIA  ) ? "CIAイメージオプション..."  :
        (filetype & GAME_NCSD ) ? "NCSDイメージオプション..." :
        (filetype & GAME_NCCH ) ? "NCCHイメージオプション..." :
        (filetype & GAME_EXEFS) ? "EXEFSイメージを接続"  :
        (filetype & GAME_ROMFS) ? "ROMFSイメージを接続"  :
        (filetype & GAME_TMD  ) ? "TMD ファイル　オプション..."   :
        (filetype & GAME_CDNTMD)? "TMD/CDN オプション..."    :
        (filetype & GAME_TWLTMD)? "TMD/TWL オプション..."    :
        (filetype & GAME_TIE  ) ? "タイトル管理..."       :
        (filetype & GAME_BOSS ) ? "BOSSファイルオプション..."  :
        (filetype & GAME_NUSCDN)? "NUS/CDNファイルを複合化"  :
        (filetype & GAME_SMDH)  ? "SMDHのタイトル情報を表示"  :
        (filetype & GAME_NDS)   ? "NDS イメージオプション..."  :
        (filetype & GAME_GBA)   ? "GBA イメージオプション..."  :
        (filetype & GAME_TICKET)? "Ticketオプション..."     :
        (filetype & GAME_TAD)   ? "TAD イメージオプション..."  :
        (filetype & GAME_3DSX)  ? "3DSXのタイトル情報を表示する"  :
        (filetype & SYS_FIRM  ) ? "FIRM イメージオプション..." :
        (filetype & SYS_AGBSAVE)? (agbimportable) ? "AGBSAVE オプション..." : "GBAのVCセーブをダンプ" :
        (filetype & SYS_TICKDB) ? "Ticket.db オプション..."  :
        (filetype & SYS_DIFF)   ? "DIFFイメージを接続"   :
        (filetype & SYS_DISA)   ? "DISAイメージを接続"   :
        (filetype & BIN_CIFNSH) ? "cifinish.binのインストール" :
        (filetype & BIN_TIKDB)  ? "タイトルキー オプション..."   :
        (filetype & BIN_KEYDB)  ? "AESkeydb オプション..."   :
        (filetype & BIN_LEGKEY) ? "構築 " KEYDB_NAME     :
        (filetype & BIN_NCCHNFO)? "NCCHinfo オプション..."   :
        (filetype & TXT_SCRIPT) ? "GM9スクリプトの実行"    :
        (FTYPE_FONT(filetype))  ? "フォント オプション..."       :
        (filetype & GFX_PNG)    ? "PNGファイルを見る"         :
        (filetype & HDR_NAND)   ? "NCSDヘッダーの再構築"   :
        (filetype & NOIMG_NAND) ? "NCSDヘッダーの再構築" : "???";
    optionstr[hexviewer-1] = "Hexeditorで表示する";
    optionstr[calcsha256-1] = "SHA-256を計算する";
    optionstr[calcsha1-1] = "SHA-1を計算する";
    optionstr[fileinfo-1] = "ファイル情報を表示する";
    if (textviewer > 0) optionstr[textviewer-1] = "Textviewerで表示する";
    if (calccmac > 0) optionstr[calccmac-1] = "CMACを計算する";
    if (copystd > 0) optionstr[copystd-1] = "コピー先 " OUTPUT_PATH;
    if (inject > 0) optionstr[inject-1] = "インジェクトデータ @offset";
    if (searchdrv > 0) optionstr[searchdrv-1] = "保存しているフォルダを開く";
    if (titleman > 0) optionstr[titleman-1] = "タイトルフォルダを開く";

    int user_select = ShowSelectPrompt(n_opt, optionstr, (n_marked > 1) ?
        "%s%0.0s\n(%lu 選択されたファイル)" : "%s%s", pathstr, tidstr, n_marked);
    if (user_select == hexviewer) { // -> show in hex viewer
        FileHexViewer(file_path);
        GetDirContents(current_dir, current_path);
        return 0;
    }
    else if (user_select == textviewer) { // -> show in text viewer
        FileTextViewer(file_path, scriptable);
        return 0;
    }
    else if (user_select == calcsha256) { // -> calculate SHA-256
        ShaCalculator(file_path, false);
        GetDirContents(current_dir, current_path);
        return 0;
    }
    else if (user_select == calcsha1) { // -> calculate SHA-1
        ShaCalculator(file_path, true);
        GetDirContents(current_dir, current_path);
        return 0;
    }
    else if (user_select == calccmac) { // -> calculate CMAC
        optionstr[0] = "CMACのみカレントチェック";
        optionstr[1] = "すべてのCMACを検証する";
        optionstr[2] = "すべてのCMACを修正";
        user_select = (n_marked > 1) ? ShowSelectPrompt(3, optionstr, "%s\n(%lu 選択されたファイル)", pathstr, n_marked) : 1;
        if (user_select == 1) {
            CmacCalculator(file_path);
            return 0;
        } else if ((user_select == 2) || (user_select == 3)) {
            bool fix = (user_select == 3);
            u32 n_processed = 0;
            u32 n_success = 0;
            u32 n_fixed = 0;
            u32 n_nocmac = 0;
            for (u32 i = 0; i < current_dir->n_entries; i++) {
                const char* path = current_dir->entry[i].path;
                if (!current_dir->entry[i].marked) continue;
                if (!ShowProgress(n_processed++, n_marked, path)) break;
                if (CheckCmacPath(path) != 0) {
                    n_nocmac++;
                    continue;
                }
                if (CheckFileCmac(path) == 0) n_success++;
                else if (fix && (FixFileCmac(path, true) == 0)) n_fixed++;
                else { // on failure: set cursor on failed file
                    *cursor = i;
                    continue;
                }
                current_dir->entry[i].marked = false;
            }
            if (n_fixed) {
                if (n_nocmac) ShowPrompt(false, "%lu/%lu/%lu ファイルok/修正済/合計\n%lu/%lu はCMACがありません",
                    n_success, n_fixed, n_marked, n_nocmac, n_marked);
                 else ShowPrompt(false, "%lu/%lu files verified ok\n%lu/%lu files fixed",
                    n_success, n_marked, n_fixed, n_marked);
            } else {
                if (n_nocmac) ShowPrompt(false, "%lu/%lu 確認されました\n%lu/%lu はCMACがありません",
                    n_success, n_marked, n_nocmac, n_marked);
                else ShowPrompt(false, "%lu/%lu 確認されました", n_success, n_marked);
            }
            return 0;
        }
        return FileHandlerMenu(current_path, cursor, scroll, pane);
    }
    else if (user_select == fileinfo) { // -> show file info
        DirFileAttrMenu(file_path, file_name);
        return 0;
    }
    else if (user_select == copystd) { // -> copy to OUTPUT_PATH
        StandardCopy(cursor, scroll);
        return 0;
    }
    else if (user_select == inject) { // -> inject data from clipboard
        char origstr[UTF_BUFFER_BYTESIZE(18)];
        TruncateString(origstr, clipboard->entry[0].name, 18, 10);
        u64 offset = ShowHexPrompt(0, 8, "%s からデータを導入しますか?\nオフセットは下記でご指定ください。", origstr);
        if (offset != (u64) -1) {
            if (!FileInjectFile(file_path, clipboard->entry[0].path, (u32) offset, 0, 0, NULL))
                ShowPrompt(false, "導入の失敗 %s", origstr);
            clipboard->n_entries = 0;
        }
        return 0;
    }
    else if ((user_select == searchdrv) || (user_select == titleman)) { // -> open containing path
        char temp_path[256];
        if (user_select == searchdrv) strncpy(temp_path, file_path, 256);
        else if (GetTieContentPath(temp_path, file_path) != 0) return 0;

        char* last_slash = strrchr(temp_path, '/');
        if (last_slash) {
            if (N_PANES) { // switch to next pane
                memcpy((*pane)->path, current_path, 256);  // store current pane state
                (*pane)->cursor = *cursor;
                (*pane)->scroll = *scroll;
                if (++*pane >= panedata + N_PANES) *pane -= N_PANES;
            }
            snprintf(current_path, last_slash - temp_path + 1, "%s", temp_path);
            GetDirContents(current_dir, current_path);
            *scroll = 0;
            for (*cursor = 1; *cursor < current_dir->n_entries; (*cursor)++) {
                DirEntry* entry = &(current_dir->entry[*cursor]);
                if (strncasecmp(entry->path, temp_path, 256) == 0) break;
            }
            if (*cursor >= current_dir->n_entries)
                *cursor = 1;
        }
        return 0;
    }
    else if (user_select != special) {
        return 1;
    }

    // stuff for special menu starts here
    n_opt = 0;
    int show_info = (titleinfo) ? ++n_opt : -1;
    int mount = (mountable) ? ++n_opt : -1;
    int restore = (restorable) ? ++n_opt : -1;
    int ebackup = (ebackupable) ? ++n_opt : -1;
    int ncsdfix = (ncsdfixable) ? ++n_opt : -1;
    int decrypt = (decryptable) ? ++n_opt : -1;
    int encrypt = (encryptable) ? ++n_opt : -1;
    int cia_build = (cia_buildable) ? ++n_opt : -1;
    int cia_build_legit = (cia_buildable_legit) ? ++n_opt : -1;
    int cxi_dump = (cxi_dumpable) ? ++n_opt : -1;
    int cia_install = (cia_installable) ? ++n_opt : -1;
    int tik_install = (tik_installable) ? ++n_opt : -1;
    int tik_dump = (tik_dumpable) ? ++n_opt : -1;
    int cif_install = (cif_installable) ? ++n_opt : -1;
    int uninstall = (uninstallable) ? ++n_opt : -1;
    int tik_build_enc = (tik_buildable) ? ++n_opt : -1;
    int tik_build_dec = (tik_buildable) ? ++n_opt : -1;
    int key_build = (key_buildable) ? ++n_opt : -1;
    int verify = (verificable) ? ++n_opt : -1;
    int ctrtransfer = (transferable) ? ++n_opt : -1;
    int hsinject = (hsinjectable) ? ++n_opt : -1;
    int extrcode = (extrcodeable) ? ++n_opt : -1;
    int trim = (trimable) ? ++n_opt : -1;
    int rename = (renamable) ? ++n_opt : -1;
    int xorpad = (xorpadable) ? ++n_opt : -1;
    int xorpad_inplace = (xorpadable) ? ++n_opt : -1;
    int keyinit = (keyinitable) ? ++n_opt : -1;
    int keyinstall = (keyinstallable) ? ++n_opt : -1;
    int install = (installable) ? ++n_opt : -1;
    int boot = (bootable) ? ++n_opt : -1;
    int script = (scriptable) ? ++n_opt : -1;
    int font = (fontable) ? ++n_opt : -1;
    int view = (viewable) ? ++n_opt : -1;
    int agbexport = (agbexportable) ? ++n_opt : -1;
    int agbimport = (agbimportable) ? ++n_opt : -1;
    int setup = (setable) ? ++n_opt : -1;
    if (mount > 0) optionstr[mount-1] = (filetype & GAME_TMD) ? "CXI/NDSをドライブに搭載" : "ドライブにイメージを接続する";
    if (restore > 0) optionstr[restore-1] = "SysNANDのリストア (safe)";
    if (ebackup > 0) optionstr[ebackup-1] = "組み込みバックアップの更新";
    if (ncsdfix > 0) optionstr[ncsdfix-1] = "NCSDヘッダーの再構築";
    if (show_info > 0) optionstr[show_info-1] = "タイトル情報を表示する";
    if (decrypt > 0) optionstr[decrypt-1] = (cryptable_inplace) ? "復号化ファイル (...)" : "復号化ファイル (" OUTPUT_PATH ")";
    if (encrypt > 0) optionstr[encrypt-1] = (cryptable_inplace) ? "暗号化ファイル (...)" : "暗号化ファイル (" OUTPUT_PATH ")";
    if (cia_build > 0) optionstr[cia_build-1] = (cia_build_legit < 0) ? "ファイルからCIAを作成する" : "CIA作成 (標準)";
    if (cia_build_legit > 0) optionstr[cia_build_legit-1] = "CIA作成（正規品）";
    if (cxi_dump > 0) optionstr[cxi_dump-1] = "CXI/NDSファイルのダンプ";
    if (cia_install > 0) optionstr[cia_install-1] = "ゲームイメージのインストール";
    if (tik_install > 0) optionstr[tik_install-1] = "チケットをインストール";
    if (tik_dump > 0) optionstr[tik_dump-1] = "チケットファイルをダンプ";
    if (cif_install > 0) optionstr[cif_install-1] = "cifinish.binのインストール";
    if (uninstall > 0) optionstr[uninstall-1] = "タイトルをアンインストール";
    if (tik_build_enc > 0) optionstr[tik_build_enc-1] = "構築 " TIKDB_NAME_ENC;
    if (tik_build_dec > 0) optionstr[tik_build_dec-1] = "構築 " TIKDB_NAME_DEC;
    if (key_build > 0) optionstr[key_build-1] = "構築 " KEYDB_NAME;
    if (verify > 0) optionstr[verify-1] = "ファイルを確認";
    if (ctrtransfer > 0) optionstr[ctrtransfer-1] = "CTRNANDへのイメージを転送";
    if (hsinject > 0) optionstr[hsinject-1] = "H&Sに注入";
    if (trim > 0) optionstr[trim-1] = "タイトルをトリム";
    if (rename > 0) optionstr[rename-1] = "ファイル名変更";
    if (xorpad > 0) optionstr[xorpad-1] = "XORpadを作る (SD output)";
    if (xorpad_inplace > 0) optionstr[xorpad_inplace-1] = "XORpadを作る (置き換える)";
    if (extrcode > 0) optionstr[extrcode-1] = "抽出 " EXEFS_CODE_NAME;
    if (keyinit > 0) optionstr[keyinit-1] = "イにっと " KEYDB_NAME;
    if (keyinstall > 0) optionstr[keyinstall-1] = "インストール " KEYDB_NAME;
    if (install > 0) optionstr[install-1] = "FIRMをインストール";
    if (boot > 0) optionstr[boot-1] = "FIRMを起動";
    if (script > 0) optionstr[script-1] = "GM9スクリプトの実行";
    if (view > 0) optionstr[view-1] = "PNGファイルを見る";
    if (font > 0) optionstr[font-1] = "アクティブフォントに設定する";
    if (agbexport > 0) optionstr[agbexport-1] = "GBAのVCセーブをダンプ";
    if (agbimport > 0) optionstr[agbimport-1] = "GBA VCセーブのインジェクション";
    if (setup > 0) optionstr[setup-1] = "デフォルトで設定";

    // auto select when there is only one option
    user_select = (n_opt <= 1) ? n_opt : (int) ShowSelectPrompt(n_opt, optionstr, (n_marked > 1) ?
        "%s%0.0s\n(%lu 選択されたファイル)" : "%s%s", pathstr, tidstr, n_marked);
    if (user_select == mount) { // -> mount file as image
        const char* mnt_drv_paths[] = { "7:", "G:", "K:", "T:", "I:", "D:" }; // maybe move that to fsdrive.h
        if (clipboard->n_entries && (DriveType(clipboard->entry[0].path) & DRV_IMAGE))
            clipboard->n_entries = 0; // remove last mounted image clipboard entries
        SetTitleManagerMode(false); // disable title manager mode
        InitImgFS((filetype & GAME_TMD) ? cxi_path : file_path);

        const char* drv_path = NULL; // find path of mounted drive
        for (u32 i = 0; i < (sizeof(mnt_drv_paths) / sizeof(const char*)); i++) {
            if (DriveType((drv_path = mnt_drv_paths[i]))) break;
            drv_path = NULL;
        }

        if (!drv_path) {
            ShowPrompt(false, "イメージを接続集: 失敗");
            InitImgFS(NULL);
        } else { // open in next pane?
            if (ShowPrompt(true, "%s\nドライブとして接続 %s\nパスを入力しますか？", pathstr, drv_path)) {
                if (N_PANES) {
                    memcpy((*pane)->path, current_path, 256);  // store current pane state
                    (*pane)->cursor = *cursor;
                    (*pane)->scroll = *scroll;
                    if (++*pane >= panedata + N_PANES) *pane -= N_PANES;
                }

                strncpy(current_path, drv_path, 256);
                GetDirContents(current_dir, current_path);
                *cursor = 1;
                *scroll = 0;
            }
        }
        return 0;
    }
    else if (user_select == decrypt) { // -> decrypt game file
        if (cryptable_inplace) {
            optionstr[0] = "復号化 " OUTPUT_PATH;
            optionstr[1] = "インプレース復号化";
            user_select = (int) ShowSelectPrompt(2, optionstr, (n_marked > 1) ?
                "%s%0.0s\n(%lu 選択されたファイル)" : "%s%s", pathstr, tidstr, n_marked);
        } else user_select = 1;
        bool inplace = (user_select == 2);
        if (!user_select) { // do nothing when no choice is made
        } else if ((n_marked > 1) && ShowPrompt(true, "選択したすべてのファイルを復号化しますか？ %lu ", n_marked)) {
            u32 n_success = 0;
            u32 n_unencrypted = 0;
            u32 n_other = 0;
            ShowString(" %lu ファイルの復号化を試みています。...", n_marked);
            for (u32 i = 0; i < current_dir->n_entries; i++) {
                const char* path = current_dir->entry[i].path;
                if (!current_dir->entry[i].marked)
                    continue;
                if (!(IdentifyFileType(path) & filetype & TYPE_BASE)) {
                    n_other++;
                    continue;
                }
                if (!(filetype & BIN_KEYDB) && (CheckEncryptedGameFile(path) != 0)) {
                    n_unencrypted++;
                    continue;
                }
                DrawDirContents(current_dir, (*cursor = i), scroll);
                if (!(filetype & BIN_KEYDB) && (CryptGameFile(path, inplace, false) == 0)) n_success++;
                else if ((filetype & BIN_KEYDB) && (CryptAesKeyDb(path, inplace, false) == 0)) n_success++;
                else { // on failure: show error, continue
                    char lpathstr[UTF_BUFFER_BYTESIZE(32)];
                    TruncateString(lpathstr, path, 32, 8);
                    if (ShowPrompt(true, "%s\n復号化に失敗\n \n続けますか?", lpathstr)) continue;
                    else break;
                }
                current_dir->entry[i].marked = false;
            }
            if (n_other || n_unencrypted) {
                ShowPrompt(false, "%lu/%lu 復号化されました\n%lu/%lu 非暗号化\n%lu/%lu 種類が異なる",
                    n_success, n_marked, n_unencrypted, n_marked, n_other, n_marked);
            } else ShowPrompt(false, "%lu/%lu 復号化されました", n_success, n_marked);
            if (!inplace && n_success) ShowPrompt(false, "%lu に書き込まれたファイル。 %s", n_success, OUTPUT_PATH);
        } else {
            if (!(filetype & BIN_KEYDB) && (CheckEncryptedGameFile(file_path) != 0)) {
                ShowPrompt(false, "%s\nファイルは暗号化されていません", pathstr);
            } else {
                u32 ret = (filetype & BIN_KEYDB) ? CryptAesKeyDb(file_path, inplace, false) :
                    CryptGameFile(file_path, inplace, false);
                if (inplace || (ret != 0)) ShowPrompt(false, "%s\n復号化 %s", pathstr, (ret == 0) ? "成功" : "失敗");
                else ShowPrompt(false, "%s\n暗号化 %s", pathstr, OUTPUT_PATH);
            }
        }
        return 0;
    }
    else if (user_select == encrypt) { // -> encrypt game file
        if (cryptable_inplace) {
            optionstr[0] = "暗号化 " OUTPUT_PATH;
            optionstr[1] = "インプレースで暗号化";
            user_select = (int) ShowSelectPrompt(2, optionstr,  (n_marked > 1) ?
                "%s%0.0s\n(%lu 選択されたファイル)" : "%s%s", pathstr, tidstr, n_marked);
        } else user_select = 1;
        bool inplace = (user_select == 2);
        if (!user_select) { // do nothing when no choice is made
        } else if ((n_marked > 1) && ShowPrompt(true, "選択したすべての %lu ファイルを暗号化しますか?", n_marked)) {
            u32 n_success = 0;
            u32 n_other = 0;
            ShowString(" %lu ファイルの暗号化を試みています...", n_marked);
            for (u32 i = 0; i < current_dir->n_entries; i++) {
                const char* path = current_dir->entry[i].path;
                if (!current_dir->entry[i].marked)
                    continue;
                if (!(IdentifyFileType(path) & filetype & TYPE_BASE)) {
                    n_other++;
                    continue;
                }
                DrawDirContents(current_dir, (*cursor = i), scroll);
                if (!(filetype & BIN_KEYDB) && (CryptGameFile(path, inplace, true) == 0)) n_success++;
                else if ((filetype & BIN_KEYDB) && (CryptAesKeyDb(path, inplace, true) == 0)) n_success++;
                else { // on failure: show error, continue
                    char lpathstr[UTF_BUFFER_BYTESIZE(32)];
                    TruncateString(lpathstr, path, 32, 8);
                    if (ShowPrompt(true, "%s\n暗号化に失敗\n \n続けますか?", lpathstr)) continue;
                    else break;
                }
                current_dir->entry[i].marked = false;
            }
            if (n_other) {
                ShowPrompt(false, "%lu/%lu ファイルを暗号化しました\n%lu/%lu 種類が異なる",
                    n_success, n_marked, n_other, n_marked);
            } else ShowPrompt(false, "%lu ファイルを暗号化しました", n_success, n_marked);
            if (!inplace && n_success) ShowPrompt(false, "%lu　ファイルが　%s　に書き込まれました。", n_success, OUTPUT_PATH);
        } else {
            u32 ret = (filetype & BIN_KEYDB) ? CryptAesKeyDb(file_path, inplace, true) :
                CryptGameFile(file_path, inplace, true);
            if (inplace || (ret != 0)) ShowPrompt(false, "%s\n暗号化 %s", pathstr, (ret == 0) ? "成功" : "失敗");
            else ShowPrompt(false, "%s\n暗号化 %s", pathstr, OUTPUT_PATH);
        }
        return 0;
    }
    else if ((user_select == cia_build) || (user_select == cia_build_legit) || (user_select == cxi_dump)) { // -> build CIA / dump CXI
        char* type = (user_select == cxi_dump) ? "CXI" : "CIA";
        bool force_legit = (user_select == cia_build_legit);
        if ((n_marked > 1) && ShowPrompt(true, "選択されたすべての%luファイルを処理しようとしますか?", n_marked)) {
            u32 n_success = 0;
            u32 n_other = 0;
            for (u32 i = 0; i < current_dir->n_entries; i++) {
                const char* path = current_dir->entry[i].path;
                if (!current_dir->entry[i].marked)
                    continue;
                if (!(IdentifyFileType(path) & filetype & TYPE_BASE)) {
                    n_other++;
                    continue;
                }
                DrawDirContents(current_dir, (*cursor = i), scroll);
                if (((user_select != cxi_dump) && (BuildCiaFromGameFile(path, force_legit) == 0)) ||
                    ((user_select == cxi_dump) && (DumpCxiSrlFromGameFile(path) == 0))) n_success++;
                else { // on failure: show error, continue
                    char lpathstr[UTF_BUFFER_BYTESIZE(32)];
                    TruncateString(lpathstr, path, 32, 8);
                    if (ShowPrompt(true, "%s\n　%s 構築に失敗しました\n \n続けますか?", lpathstr, type)) continue;
                    else break;
                }
                current_dir->entry[i].marked = false;
            }
            if (n_other) ShowPrompt(false, "%lu/%lu %ss 構築しました\n%lu/%lu 種類が異なる",
                n_success, n_marked, type, n_other, n_marked);
            else ShowPrompt(false, "%lu/%lu %ss 構築しました", n_success, n_marked, type);
            if (n_success) ShowPrompt(false, "%lu　ファイルが　%s　に書き込まれました。", n_success, OUTPUT_PATH);
            if (n_success && in_output_path) GetDirContents(current_dir, current_path);
            if (n_success != (n_marked - n_other)) {
                ShowPrompt(false, "%luァイルは変換に失敗しました。\n検証を推奨します。",
                    n_marked - (n_success + n_other));
            }
        } else {
            if (((user_select != cxi_dump) && (BuildCiaFromGameFile(file_path, force_legit) == 0)) ||
                ((user_select == cxi_dump) && (DumpCxiSrlFromGameFile(file_path) == 0))) {
                ShowPrompt(false, "%s\n%s 構築 %s", pathstr, type, OUTPUT_PATH);
                if (in_output_path) GetDirContents(current_dir, current_path);
            } else {
                ShowPrompt(false, "%s\n%s 構築に失敗しました", pathstr, type);
                if ((filetype & (GAME_NCCH|GAME_NCSD)) &&
                    ShowPrompt(true, "%s\nファイルの変換に失敗しました。\n \n今すぐ検証しますか？", pathstr)) {
                    ShowPrompt(false, "%s\n確認 %s", pathstr,
                        (VerifyGameFile(file_path) == 0) ? "成功" : "失敗");
                }
            }
        }
        return 0;
    }
    else if ((user_select == cia_install) || (user_select == tik_install) ||
             (user_select == cif_install)) { // -> install game/ticket/cifinish file
        u32 (*InstallFunction)(const char*, bool) =
            (user_select == cia_install) ? &InstallGameFile :
            (user_select == tik_install) ? &InstallTicketFile : &InstallCifinishFile;
        bool to_emunand = false;
        if (CheckVirtualDrive("E:")) {
            optionstr[0] = "SysNANDにインストール";
            optionstr[1] = "EmuNANDにインストール";
            user_select = (int) ShowSelectPrompt(2, optionstr,  (n_marked > 1) ?
                "%s%0.0s\n(%lu 選択されたファイル)" : "%s%s", pathstr, tidstr, n_marked);
            if (!user_select) return 0;
            else to_emunand = (user_select == 2);
        }
        if ((n_marked > 1) && ShowPrompt(true, "選択したすべての %lu ファイルをインストールしますか?", n_marked)) {
            u32 n_success = 0;
            u32 n_other = 0;
            ShowString("%lu　ファイルのインストールを試みています。...", n_marked);
            for (u32 i = 0; i < current_dir->n_entries; i++) {
                const char* path = current_dir->entry[i].path;
                if (!current_dir->entry[i].marked)
                    continue;
                if (!(IdentifyFileType(path) & filetype & TYPE_BASE)) {
                    n_other++;
                    continue;
                }
                DrawDirContents(current_dir, (*cursor = i), scroll);
                if ((*InstallFunction)(path, to_emunand) == 0)
                    n_success++;
                else { // on failure: show error, continue
                    char lpathstr[UTF_BUFFER_BYTESIZE(32)];
                    TruncateString(lpathstr, path, 32, 8);
                    if (ShowPrompt(true, "%s\nインストールに失敗しました\n \n続けますか?", lpathstr)) continue;
                    else break;
                }
                current_dir->entry[i].marked = false;
            }
            if (n_other) {
                ShowPrompt(false, "%lu/%lu ファイルをインストールしました\n%lu/%lu 種類が異なる",
                    n_success, n_marked, n_other, n_marked);
            } else ShowPrompt(false, "%lu/%lu ファイルをインストールしました", n_success, n_marked);
        } else {
            u32 ret = (*InstallFunction)(file_path, to_emunand);
            ShowPrompt(false, "%s\nInstall %s", pathstr, (ret == 0) ? "成功" : "失敗");
            if ((ret != 0) && (filetype & (GAME_NCCH|GAME_NCSD)) &&
                ShowPrompt(true, "%s\nファイルのインストールに失敗しました\n \n今すぐ検証しますか？", pathstr)) {
                ShowPrompt(false, "%s\n検証 %s", pathstr,
                    (VerifyGameFile(file_path) == 0) ? "成功" : "失敗");
            }
        }
        return 0;
    }
    else if (user_select == uninstall) { // -> uninstall title
        bool full_uninstall = false;

        // safety confirmation
        optionstr[0] = "チケットとセーブを保持する";
        optionstr[1] = "すべてをアンインストール";
        optionstr[2] = "アンインストールを中止する";
        user_select = (int) (n_marked > 1) ?
            ShowSelectPrompt(3, optionstr, "選択されたタイトル %lu をアンインストールしますか？", n_marked) :
            ShowSelectPrompt(3, optionstr, "%s\n選択したタイトルをアンインストールしますか？", pathstr);
        full_uninstall = (user_select == 2);
        if (!user_select || (user_select == 3))
            return 0;

        // batch uninstall
        if (n_marked > 1) {
            u32 n_success = 0;
            u32 num = 0;
            for (u32 i = 0; i < current_dir->n_entries; i++) {
                const char* path = current_dir->entry[i].path;
                if (!current_dir->entry[i].marked) continue;
                if (!(IdentifyFileType(path) & filetype & TYPE_BASE)) continue;
                if (!num && !CheckWritePermissions(path)) break;
                if (!ShowProgress(num++, n_marked, path)) break;
                if (UninstallGameDataTie(path, true, full_uninstall, full_uninstall) == 0)
                    n_success++;
            }
            ShowPrompt(false, "%lu/%lu アンインストールされたタイトル", n_success, n_marked);
        } else if (CheckWritePermissions(file_path)) {
            ShowString("%s\nアンインストール中です、しばらくお待ちください...", pathstr);
            if (UninstallGameDataTie(file_path, true, full_uninstall, full_uninstall) != 0)
                ShowPrompt(false, "%s\nアンインストールに失敗しました!", pathstr);
            ClearScreenF(true, false, COLOR_STD_BG);
        }

        GetDirContents(current_dir, current_path);
        return 0;
    }
    else if (user_select == verify) { // -> verify game / nand file
        if ((n_marked > 1) && ShowPrompt(true, "選択されたすべての %lu ファイルを検証しますか?", n_marked)) {
            u32 n_success = 0;
            u32 n_other = 0;
            u32 n_processed = 0;
            for (u32 i = 0; i < current_dir->n_entries; i++) {
                const char* path = current_dir->entry[i].path;
                if (!current_dir->entry[i].marked)
                    continue;
                if (!(filetype & (GAME_CIA|GAME_TMD|GAME_NCSD|GAME_NCCH)) &&
                    !ShowProgress(n_processed++, n_marked, path)) break;
                if (!(IdentifyFileType(path) & filetype & TYPE_BASE)) {
                    n_other++;
                    continue;
                }
                DrawDirContents(current_dir, (*cursor = i), scroll);
                if ((filetype & IMG_NAND) && (ValidateNandDump(path) == 0)) n_success++;
                else if (VerifyGameFile(path) == 0) n_success++;
                else { // on failure: show error, continue
                    char lpathstr[UTF_BUFFER_BYTESIZE(32)];
                    TruncateString(lpathstr, path, 32, 8);
                    if (ShowPrompt(true, "%s\n検証に失敗しました\n \n続けますか?", lpathstr)) {
                        if (!(filetype & (GAME_CIA|GAME_TMD|GAME_NCSD|GAME_NCCH)))
                            ShowProgress(0, n_marked, path); // restart progress bar
                        continue;
                    } else break;
                }
                current_dir->entry[i].marked = false;
            }
            if (n_other) ShowPrompt(false, "%lu/%lu 確認されました\n%lu/%lu 種類が異なる",
                n_success, n_marked, n_other, n_marked);
            else ShowPrompt(false, "%lu/%lu 確認されました", n_success, n_marked);
        } else {
            ShowString("%s\nファイルを検証中です、しばらくお待ちください...", pathstr);
            if (filetype & IMG_NAND) {
                ShowPrompt(false, "%s\nNANDの検証 %s", pathstr,
                    (ValidateNandDump(file_path) == 0) ? "成功" : "失敗");
            } else ShowPrompt(false, "%s\n検証 %s", pathstr,
                (VerifyGameFile(file_path) == 0) ? "成功" : "失敗");
        }
        return 0;
    }
    else if (user_select == tik_dump) { // dump ticket file
        if ((n_marked > 1) && ShowPrompt(true, "選択されたすべての %lu ファイルをダンプしますか？", n_marked)) {
            u32 n_success = 0;
            u32 n_legit = 0;
            bool force_legit = true;
            for (u32 n_processed = 0;; n_processed = 0) {
                for (u32 i = 0; i < current_dir->n_entries; i++) {
                    const char* path = current_dir->entry[i].path;
                    if (!current_dir->entry[i].marked) continue;
                    if (!ShowProgress(n_processed++, n_marked, path)) break;
                    DrawDirContents(current_dir, (*cursor = i), scroll);
                    if (DumpTicketForGameFile(path, force_legit) == 0) n_success++;
                    else if (IdentifyFileType(path) & filetype & TYPE_BASE) continue;
                    if (force_legit) n_legit++;
                    current_dir->entry[i].marked = false;
                }
                if (force_legit && (n_success != n_marked))
                    if (!ShowPrompt(true, "%lu/%lu 正規のチケットダンプ\n \n全チケットをダンプしますか?",
                        n_legit, n_marked)) break;
                if (!force_legit) break;
                force_legit = false;
            }
            ShowPrompt(false, "%lu/%lu ダンプされました %s",
                n_success, n_marked, OUTPUT_PATH);
        } else {
            if (DumpTicketForGameFile(file_path, true) == 0) {
                ShowPrompt(false, "%s\nチケットをダンプ %s", pathstr, OUTPUT_PATH);
            } else if (ShowPrompt(false, "%s\n正規のチケットが見つかりません。\n \nダンプしますか?", pathstr)) {
                if (DumpTicketForGameFile(file_path, false) == 0)
                    ShowPrompt(false, "%s\nチケットをダンプ %s", pathstr, OUTPUT_PATH);
                else ShowPrompt(false, "%s\nチケットのダンプに失敗しました!", pathstr);
            }
        }
        return 0;
    }
    else if ((user_select == tik_build_enc) || (user_select == tik_build_dec)) { // -> (re)build titlekey database
        bool dec = (user_select == tik_build_dec);
        const char* path_out = (dec) ? OUTPUT_PATH "/" TIKDB_NAME_DEC : OUTPUT_PATH "/" TIKDB_NAME_ENC;
        if (BuildTitleKeyInfo(NULL, dec, false) != 0) return 1; // init database
        ShowString("構築中 %s...", (dec) ? TIKDB_NAME_DEC : TIKDB_NAME_ENC);
        if (n_marked > 1) {
            u32 n_success = 0;
            u32 n_other = 0;
            for (u32 i = 0; i < current_dir->n_entries; i++) {
                const char* path = current_dir->entry[i].path;
                if (!current_dir->entry[i].marked)
                    continue;
                if (!FTYPE_TIKBUILD(IdentifyFileType(path))) {
                    n_other++;
                    continue;
                }
                current_dir->entry[i].marked = false;
                if (BuildTitleKeyInfo(path, dec, false) == 0) n_success++; // ignore failures for now
            }
            if (BuildTitleKeyInfo(NULL, dec, true) == 0) {
                if (n_other) ShowPrompt(false, "%s\n%lu/%lu 処理済みファイル\n%lu/%lu ファイル無視",
                    path_out, n_success, n_marked, n_other, n_marked);
                else ShowPrompt(false, "%s\n%lu/%lu 処理済みファイル", path_out, n_success, n_marked);
            } else ShowPrompt(false, "%s\nデータベースの構築に失敗しました。", path_out);
        } else ShowPrompt(false, "%s\nデータベース構築 %s.", path_out,
            (BuildTitleKeyInfo(file_path, dec, true) == 0) ? "成功" : "失敗");
        return 0;
    }
    else if (user_select == key_build) { // -> (Re)Build AES key database
        const char* path_out = OUTPUT_PATH "/" KEYDB_NAME;
        if (BuildKeyDb(NULL, false) != 0) return 1; // init database
        ShowString("構築中 %s...", KEYDB_NAME);
        if (n_marked > 1) {
            u32 n_success = 0;
            u32 n_other = 0;
            for (u32 i = 0; i < current_dir->n_entries; i++) {
                const char* path = current_dir->entry[i].path;
                if (!current_dir->entry[i].marked)
                    continue;
                if (!FTYPE_KEYBUILD(IdentifyFileType(path))) {
                    n_other++;
                    continue;
                }
                current_dir->entry[i].marked = false;
                if (BuildKeyDb(path, false) == 0) n_success++; // ignore failures for now
            }
            if (BuildKeyDb(NULL, true) == 0) {
                if (n_other) ShowPrompt(false, "%s\n%lu/%lu 処理済みファイル\n%lu/%lu ファイル無視",
                    path_out, n_success, n_marked, n_other, n_marked);
                else ShowPrompt(false, "%s\n%lu/%lu 処理済みファイル", path_out, n_success, n_marked);
            } else ShowPrompt(false, "%s\nデータベースの構築に失敗しました。", path_out);
        } else ShowPrompt(false, "%s\nデータベース構築 %s.", path_out,
            (BuildKeyDb(file_path, true) == 0) ? "成功" : "失敗");
        return 0;
    }
    else if (user_select == trim) { // -> Game file trimmer
        if ((n_marked > 1) && ShowPrompt(true, "選択されたすべての %lu ファイルをトリミングしますか?", n_marked)) {
            u32 n_success = 0;
            u32 n_other = 0;
            u32 n_processed = 0;
            u64 savings = 0;
            char savingsstr[32];
            for (u32 i = 0; i < current_dir->n_entries; i++) {
                const char* path = current_dir->entry[i].path;
                u64 prevsize = 0;
                if (!current_dir->entry[i].marked)
                    continue;
                if (!ShowProgress(n_processed++, n_marked, path)) break;
                if (!(IdentifyFileType(path) & filetype & TYPE_BASE)) {
                    n_other++;
                    continue;
                }
                prevsize = FileGetSize(path);
                if (TrimGameFile(path) == 0) {
                    n_success++;
                    savings += prevsize - FileGetSize(path);
                } else { // on failure: show error, continue (should not happen)
                    char lpathstr[UTF_BUFFER_BYTESIZE(32)];
                    TruncateString(lpathstr, path, 32, 8);
                    if (ShowPrompt(true, "%s\nトリミングの失敗\n \n続けますか?", lpathstr)) {
                        ShowProgress(0, n_marked, path); // restart progress bar
                        continue;
                    } else break;
                }
                current_dir->entry[i].marked = false;
            }
            FormatBytes(savingsstr, savings);
            if (n_other) ShowPrompt(false, "%lu/%lu トリミングされました\n%lu/%lu 種類が異なる\n%s 保存されました!",
                n_success, n_marked, n_other, n_marked, savingsstr);
            else ShowPrompt(false, "%lu/%lu トリミングされました\n%s 保存されました", n_success, n_marked, savingsstr);
            if (n_success) GetDirContents(current_dir, current_path);
        } else {
            u64 trimsize = GetGameFileTrimmedSize(file_path);
            u64 currentsize = FileGetSize(file_path);
            char tsizestr[32];
            char csizestr[32];
            char dsizestr[32];
            FormatBytes(tsizestr, trimsize);
            FormatBytes(csizestr, currentsize);
            FormatBytes(dsizestr, currentsize - trimsize);

            if (!trimsize || trimsize > currentsize) {
                ShowPrompt(false, "%s\nファイルのトリミングはできません。", pathstr);
            } else if (trimsize == currentsize) {
                ShowPrompt(false, "%s\nファイルはトリミング済です。", pathstr);
            } else if (ShowPrompt(true, "%s\n現在の容量: %s\nトリムサイズ: %s\n違い: %s\n \nファイルをトリムしますか?",
                pathstr, csizestr, tsizestr, dsizestr)) {
                if (TrimGameFile(file_path) != 0) ShowPrompt(false, "%s\nトリムに失敗しました", pathstr);
                else {
                    ShowPrompt(false, "%s\nトリムは %sで行っています", pathstr, dsizestr);
                    GetDirContents(current_dir, current_path);
                }
            }
        }
        return 0;
    }
    else if (user_select == rename) { // -> Game file renamer
        if ((n_marked > 1) && ShowPrompt(true, "選択したすべての %lu ファイルの名前を変更しますか?", n_marked)) {
            u32 n_success = 0;
            ShowProgress(0, 0, "");
            for (u32 i = 0; i < current_dir->n_entries; i++) {
                DirEntry* entry = &(current_dir->entry[i]);
                if (!current_dir->entry[i].marked) continue;
                ShowProgress(i+1, current_dir->n_entries, entry->name);
                if (!GoodRenamer(entry, false)) continue;
                n_success++;
                current_dir->entry[i].marked = false;
            }
            ShowPrompt(false, "%lu/%lu 名前が変更されました", n_success, n_marked);
        } else if (!GoodRenamer(&(current_dir->entry[*cursor]), true)) {
            ShowPrompt(false, "%s\n良い名前に変更できませんでした", pathstr);
        }
        return 0;
    }
    else if (user_select == show_info) { // -> Show title info
        ShowGameCheckerInfo(file_path);
        return 0;
    }
    else if (user_select == hsinject) { // -> Inject to Health & Safety
        char* destdrv[2] = { NULL };
        n_opt = 0;
        if (DriveType("1:")) {
            optionstr[n_opt] = "SysNAND H&Sインジェクト";
            destdrv[n_opt++] = "1:";
        }
        if (DriveType("4:")) {
            optionstr[n_opt] = "EmuNAND H&Sインジェクト";
            destdrv[n_opt++] = "4:";
        }
        user_select = (n_opt > 1) ? (int) ShowSelectPrompt(n_opt, optionstr, "%s", pathstr) : n_opt;
        if (user_select) {
            ShowPrompt(false, "%s\nH&Sインジェクト %s", pathstr,
                (InjectHealthAndSafety(file_path, destdrv[user_select-1]) == 0) ? "成功" : "失敗");
        }
        return 0;
    }
    else if (user_select == extrcode) { // -> Extract .code
        if ((n_marked > 1) && ShowPrompt(true, "選択されたすべての %lu ファイルを展開しますか?", n_marked)) {
            u32 n_success = 0;
            u32 n_other = 0;
            u32 n_processed = 0;
            for (u32 i = 0; i < current_dir->n_entries; i++) {
                const char* path = current_dir->entry[i].path;
                if (!current_dir->entry[i].marked)
                    continue;
                if (!ShowProgress(n_processed++, n_marked, path)) break;
                if (!(IdentifyFileType(path) & filetype & TYPE_BASE)) {
                    n_other++;
                    continue;
                }
                DrawDirContents(current_dir, (*cursor = i), scroll);
                if (filetype & GAME_TMD) {
                    char cxi_pathl[256] = { 0 };
                    if ((GetTmdContentPath(cxi_pathl, path) == 0) && PathExist(cxi_pathl) &&
                        (ExtractCodeFromCxiFile(cxi_pathl, NULL, NULL) == 0)) {
                        n_success++;
                    } else continue;
                } else {
                    if (ExtractCodeFromCxiFile(path, NULL, NULL) == 0) n_success++;
                    else continue;
                }
                current_dir->entry[i].marked = false;
            }
            if (n_other) ShowPrompt(false, "%lu/%lu 展開できました\n%lu/%lu 種類が異なる",
                n_success, n_marked, n_other, n_marked);
            else ShowPrompt(false, "%lu/%lu 展開できました", n_success, n_marked);
        } else {
            char extstr[8] = { 0 };
            ShowString("%s\n.codeを展開中です、しばらくお待ちください。...", pathstr);
            if (ExtractCodeFromCxiFile((filetype & GAME_TMD) ? cxi_path : file_path, NULL, extstr) == 0) {
                ShowPrompt(false, "%s\n%s 展開 " OUTPUT_PATH, pathstr, extstr);
            } else ShowPrompt(false, "%s\n.codeの抽出に失敗しました", pathstr);
        }
        return 0;
    }
    else if (user_select == ctrtransfer) { // -> transfer CTRNAND image to SysNAND
        char* destdrv[2] = { NULL };
        n_opt = 0;
        if (DriveType("1:")) {
            optionstr[n_opt] = "SysNANDへの転送";
            destdrv[n_opt++] = "1:";
        }
        if (DriveType("4:")) {
            optionstr[n_opt] = "EmuNANDへの転送";
            destdrv[n_opt++] = "4:";
        }
        if (n_opt) {
            user_select = (n_opt > 1) ? (int) ShowSelectPrompt(n_opt, optionstr, "%s", pathstr) : 1;
            if (user_select) {
                ShowPrompt(false, "%s\nCTRNAND転送 %s", pathstr,
                    (TransferCtrNandImage(file_path, destdrv[user_select-1]) == 0) ? "成功" : "失敗");
            }
        } else ShowPrompt(false, "%s\n有効な宛先が見つかりません", pathstr);
        return 0;
    }
    else if (user_select == restore) { // -> restore SysNAND (A9LH preserving)
        ShowPrompt(false, "%s\nNANDリストア %s", pathstr,
            (SafeRestoreNandDump(file_path) == 0) ? "成功" : "失敗");
        return 0;
    }
    else if (user_select == ncsdfix) { // -> inject sighaxed NCSD
        ShowPrompt(false, "%s\nNCSDを再構築 %s", pathstr,
            (FixNandHeader(file_path, !(filetype == HDR_NAND)) == 0) ? "成功" : "失敗");
        GetDirContents(current_dir, current_path);
        InitExtFS(); // this might have fixed something, so try this
        return 0;
    }
    else if ((user_select == xorpad) || (user_select == xorpad_inplace)) { // -> build xorpads
        bool inplace = (user_select == xorpad_inplace);
        bool success = (BuildNcchInfoXorpads((inplace) ? current_path : OUTPUT_PATH, file_path) == 0);
        ShowPrompt(false, "%s\nNCCHinfo padgen %s%s", pathstr,
            (success) ? "成功" : "失敗",
            (!success || inplace) ? "" : "\n出力先: " OUTPUT_PATH);
        GetDirContents(current_dir, current_path);
        for (; *cursor < current_dir->n_entries; (*cursor)++) {
            DirEntry* entry = &(current_dir->entry[*cursor]);
            if (strncasecmp(entry->name, NCCHINFO_NAME, 32) == 0) break;
        }
        if (*cursor >= current_dir->n_entries) {
            *scroll = 0;
            *cursor = 1;
        }
        return 0;
    }
    else if (user_select == ebackup) { // -> update embedded backup
        ShowString("%s\n埋め込みバックアップの更新...", pathstr);
        bool required = (CheckEmbeddedBackup(file_path) != 0);
        bool success = (required && (EmbedEssentialBackup(file_path) == 0));
        ShowPrompt(false, "%s\nバックアップ更新: %s", pathstr, (!required) ? "必要なし" :
            (success) ? "完了" : "失敗!");
        GetDirContents(current_dir, current_path);
        return 0;
    }
    else if (user_select == keyinit) { // -> initialise keys from aeskeydb.bin
        if (ShowPrompt(true, "警告: キーは検証されていません。\n自己責任で続けますか"))
            ShowPrompt(false, "%s\nAESkeydb 起動 %s", pathstr, (InitKeyDb(file_path) == 0) ? "成功" : "失敗");
        return 0;
    }
    else if (user_select == keyinstall) { // -> install keys from aeskeydb.bin
        ShowPrompt(false, "%s\nAESkeydbインストール %s", pathstr, (SafeInstallKeyDb(file_path) == 0) ? "成功" : "失敗");
        return 0;
    }
    else if (user_select == install) { // -> install FIRM
        size_t firm_size = FileGetSize(file_path);
        u32 slots = 1;
        if (GetNandPartitionInfo(NULL, NP_TYPE_FIRM, NP_SUBTYPE_CTR, 1, NAND_SYSNAND) == 0) {
            optionstr[0] = "FIRM0にインストール";
            optionstr[1] = "FIRM1にインストール";
            optionstr[2] = "両方にインストール";
            // this only works up to FIRM1
            slots = ShowSelectPrompt(3, optionstr, "%s (%dkB)\nSysNANDにインストールしますか?", pathstr, firm_size / 1024);
        } else slots = ShowPrompt(true, "%s (%dkB)\nSysNANDにインストールしますか?", pathstr, firm_size / 1024) ? 1 : 0;
        if (slots) ShowPrompt(false, "%s (%dkB)\nインストール %s", pathstr, firm_size / 1024,
            (SafeInstallFirm(file_path, slots) == 0) ? "成功" : "失敗!");
        return 0;
    }
    else if (user_select == boot) { // -> boot FIRM
        BootFirmHandler(file_path, true, false);
        return 0;
    }
    else if (user_select == script) { // execute script
        if (ShowPrompt(true, "%s\n警告: 信頼できないソースから\nスクリプトを実行しないでください。\n \nスクリプトを実行しますか？", pathstr))
            ShowPrompt(false, "%s\nスクリプトの実行 %s", pathstr, ExecuteGM9Script(file_path) ? "成功" : "失敗");
        GetDirContents(current_dir, current_path);
        ClearScreenF(true, true, COLOR_STD_BG);
        return 0;
    }
    else if (user_select == font) { // set font
        u8* font = (u8*) malloc(0x20000); // arbitrary, should be enough by far
        if (!font) return 1;
        u32 font_size = FileGetData(file_path, font, 0x20000, 0);
        if (font_size) SetFont(font, font_size);
        ClearScreenF(true, true, COLOR_STD_BG);
        free(font);
        return 0;
    }
    else if (user_select == view) { // view gfx
        if (FileGraphicsViewer(file_path) != 0)
            ShowPrompt(false, "%s\nエラー: ファイルを表示できません\n(ヒント: 大きすぎるかもしれない)", pathstr);
        return 0;
    }
    else if (user_select == agbexport) { // export GBA VC save
        if (DumpGbaVcSavegame(file_path) == 0)
            ShowPrompt(false, "ゲームのセーブをダンプ " OUTPUT_PATH ".");
        else ShowPrompt(false, "ゲームのセーブのダンプに失敗しました!");
        return 0;
    }
    else if (user_select == agbimport) { // import GBA VC save
        if (clipboard->n_entries != 1) {
            ShowPrompt(false, "GBA VCのセーブは、\nクリップボードにある必要があります。");
        } else {
            ShowPrompt(false, "ゲームセーブ導入 %s.",
                (InjectGbaVcSavegame(file_path, clipboard->entry[0].path) == 0) ? "成功" : "失敗!");
            clipboard->n_entries = 0;
        }
        return 0;
    }
    else if (user_select == setup) { // set as default (font)
        if (filetype & FONT_RIFF) {
            if (SetAsSupportFile("font.frf", file_path))
                ShowPrompt(false, "%s\n次回の起動時にフォントが有効になります", pathstr);
        } else if (filetype & FONT_PBM) {
            if (SetAsSupportFile("font.pbm", file_path))
                ShowPrompt(false, "%s\n次回の起動時にフォントが有効になります。", pathstr);
        }
        return 0;
    }

    return FileHandlerMenu(current_path, cursor, scroll, pane);
}

u32 HomeMoreMenu(char* current_path) {
    NandPartitionInfo np_info;
    if (GetNandPartitionInfo(&np_info, NP_TYPE_BONUS, NP_SUBTYPE_CTR, 0, NAND_SYSNAND) != 0) np_info.count = 0;

    const char* optionstr[8];
    const char* promptstr = "HOME その他... メニュー.\nアクションを選択:";
    u32 n_opt = 0;
    int sdformat = ++n_opt;
    int bonus = (np_info.count > 0x2000) ? (int) ++n_opt : -1; // 4MB minsize
    int multi = (CheckMultiEmuNand()) ? (int) ++n_opt : -1;
    int bsupport = ++n_opt;
    int hsrestore = ((CheckHealthAndSafetyInject("1:") == 0) || (CheckHealthAndSafetyInject("4:") == 0)) ? (int) ++n_opt : -1;
    int clock = ++n_opt;
    int bright = ++n_opt;
    int calib = ++n_opt;
    int sysinfo = ++n_opt;
    int readme = (FindVTarFileInfo(VRAM0_README_MD, NULL)) ? (int) ++n_opt : -1;

    if (sdformat > 0) optionstr[sdformat - 1] = "SDカードフォーマットメニュー";
    if (bonus > 0) optionstr[bonus - 1] = "Bonusドライブセットアップ";
    if (multi > 0) optionstr[multi - 1] = "EmuNAND切り替え";
    if (bsupport > 0) optionstr[bsupport - 1] = "サポートファイルの構築";
    if (hsrestore > 0) optionstr[hsrestore - 1] = "H&Sをリストア";
    if (clock > 0) optionstr[clock - 1] = "RTCの日付と時刻を設定";
    if (bright > 0) optionstr[bright - 1] = "明るさを設定";
    if (calib > 0) optionstr[calib - 1] = "タッチスクリーンのキャリブレーション";
    if (sysinfo > 0) optionstr[sysinfo - 1] = "システム情報";
    if (readme > 0) optionstr[readme - 1] = "ReadMeを表示";

    int user_select = ShowSelectPrompt(n_opt, optionstr, promptstr);
    if (user_select == sdformat) { // format SD card
        bool sd_state = CheckSDMountState();
        char slabel[DRV_LABEL_LEN] = { '\0' };
        if (clipboard->n_entries && (DriveType(clipboard->entry[0].path) & (DRV_SDCARD|DRV_ALIAS|DRV_EMUNAND|DRV_IMAGE)))
            clipboard->n_entries = 0; // remove SD clipboard entries
        GetFATVolumeLabel("0:", slabel); // get SD volume label
        DeinitExtFS();
        DeinitSDCardFS();
        if ((SdFormatMenu(slabel) == 0) || sd_state) {;
            while (!InitSDCardFS() &&
                ShowPrompt(true, "SDカードの初期化に失敗しました! 再実行しますか?"));
        }
        ClearScreenF(true, true, COLOR_STD_BG);
        AutoEmuNandBase(true);
        InitExtFS();
        GetDirContents(current_dir, current_path);
        return 0;
    }
    else if (user_select == bonus) { // setup bonus drive
        if (clipboard->n_entries && (DriveType(clipboard->entry[0].path) & (DRV_BONUS|DRV_IMAGE)))
            clipboard->n_entries = 0; // remove bonus drive clipboard entries
        if (!SetupBonusDrive()) ShowPrompt(false, "セットアップ失敗!");
        ClearScreenF(true, true, COLOR_STD_BG);
        GetDirContents(current_dir, current_path);
        return 0;
    }
    else if (user_select == multi) { // switch EmuNAND offset
        while (ShowPrompt(true, "現在のEmuNANDのオフセットは %06Xです.\n次のオフセットに切り替えますか?", GetEmuNandBase())) {
            if (clipboard->n_entries && (DriveType(clipboard->entry[0].path) & DRV_EMUNAND))
                clipboard->n_entries = 0; // remove EmuNAND clipboard entries
            DismountDriveType(DRV_EMUNAND);
            AutoEmuNandBase(false);
            InitExtFS();
        }
        GetDirContents(current_dir, current_path);
        return 0;
    }
    else if (user_select == bsupport) { // build support files
        bool tik_enc_sys = false;
        bool tik_enc_emu = false;
        if (BuildTitleKeyInfo(NULL, false, false) == 0) {
            ShowString("構築 " TIKDB_NAME_ENC " (SysNAND)...");
            tik_enc_sys = (BuildTitleKeyInfo("1:/dbs/ticket.db", false, false) == 0);
            ShowString("構築 " TIKDB_NAME_ENC " (EmuNAND)...");
            tik_enc_emu = (BuildTitleKeyInfo("4:/dbs/ticket.db", false, false) == 0);
            if (!tik_enc_sys || BuildTitleKeyInfo(NULL, false, true) != 0)
                tik_enc_sys = tik_enc_emu = false;
        }
        bool tik_dec_sys = false;
        bool tik_dec_emu = false;
        if (BuildTitleKeyInfo(NULL, true, false) == 0) {
            ShowString("構築 " TIKDB_NAME_DEC " (SysNAND)...");
            tik_dec_sys = (BuildTitleKeyInfo("1:/dbs/ticket.db", true, false) == 0);
            ShowString("構築 " TIKDB_NAME_DEC " (EmuNAND)...");
            tik_dec_emu = (BuildTitleKeyInfo("4:/dbs/ticket.db", true, false) == 0);
            if (!tik_dec_sys || BuildTitleKeyInfo(NULL, true, true) != 0)
                tik_dec_sys = tik_dec_emu = false;
        }
        bool seed_sys = false;
        bool seed_emu = false;
        if (BuildSeedInfo(NULL, false) == 0) {
            ShowString("構築 " SEEDINFO_NAME " (SysNAND)...");
            seed_sys = (BuildSeedInfo("1:", false) == 0);
            ShowString("構築 " SEEDINFO_NAME " (EmuNAND)...");
            seed_emu = (BuildSeedInfo("4:", false) == 0);
            if (!seed_sys || BuildSeedInfo(NULL, true) != 0)
                seed_sys = seed_emu = false;
        }
        ShowPrompt(false, "内蔵 " OUTPUT_PATH ":\n \n%18.18-s %s\n%18.18-s %s\n%18.18-s %s",
            TIKDB_NAME_ENC, tik_enc_sys ? tik_enc_emu ? "OK (Sys&Emu)" : "OK (Sys)" : "失敗",
            TIKDB_NAME_DEC, tik_dec_sys ? tik_dec_emu ? "OK (Sys&Emu)" : "OK (Sys)" : "失敗",
            SEEDINFO_NAME, seed_sys ? seed_emu ? "OK (Sys&Emu)" : "OK (Sys)" : "失敗");
        GetDirContents(current_dir, current_path);
        return 0;
    }
    else if (user_select == hsrestore) { // restore Health & Safety
        n_opt = 0;
        int sys = (CheckHealthAndSafetyInject("1:") == 0) ? (int) ++n_opt : -1;
        int emu = (CheckHealthAndSafetyInject("4:") == 0) ? (int) ++n_opt : -1;
        if (sys > 0) optionstr[sys - 1] = "H&Sをリストア (SysNAND)";
        if (emu > 0) optionstr[emu - 1] = "H&Sをリストア (EmuNAND)";
        user_select = (n_opt > 1) ? ShowSelectPrompt(n_opt, optionstr, promptstr) : n_opt;
        if (user_select > 0) {
            InjectHealthAndSafety(NULL, (user_select == sys) ? "1:" : "4:");
            GetDirContents(current_dir, current_path);
            return 0;
        }
    }
    else if (user_select == clock) { // RTC clock setter
        DsTime dstime;
        get_dstime(&dstime);
        if (ShowRtcSetterPrompt(&dstime, "RTCの日付と時刻を設定:")) {
            char timestr[32];
            set_dstime(&dstime);
            GetTimeString(timestr, true, true);
            ShowPrompt(false, "新しいRTCの日付と時刻は:\n%s\n \nヒント: ホームメニューのの時刻は\nRTC設定後、\n手動で調整する必要があります。",
                timestr);
        }
        return 0;
    }
    else if (user_select == bright) { // brightness config dialogue
        s32 old_brightness, new_brightness;
        if (!LoadSupportFile("gm9bright.cfg", &old_brightness, 4))
            old_brightness = BRIGHTNESS_AUTOMATIC; // auto by default
        new_brightness = ShowBrightnessConfig(old_brightness);
        if (old_brightness != new_brightness)
            SaveSupportFile("gm9bright.cfg", &new_brightness, 4);
        return 0;
    }
    else if (user_select == calib) { // touchscreen calibration
        ShowPrompt(false, "タッチスクリーンキャリブレーション %s!",
            (ShowTouchCalibrationDialog()) ? "成功" : "失敗　　　　　　　　　　　");
        return 0;
    }
    else if (user_select == sysinfo) { // Myria's system info
        char* sysinfo_txt = (char*) malloc(STD_BUFFER_SIZE);
        if (!sysinfo_txt) return 1;
        MyriaSysinfo(sysinfo_txt);
        MemTextViewer(sysinfo_txt, strnlen(sysinfo_txt, STD_BUFFER_SIZE), 1, false);
        free(sysinfo_txt);
        return 0;
    }
    else if (user_select == readme) { // Display GodMode9 readme
        u64 README_md_size;
        char* README_md = FindVTarFileInfo(VRAM0_README_MD, &README_md_size);
        MemToCViewer(README_md, README_md_size, "GodMode9 ReadMe 目次");
        return 0;
    } else return 1;

    return HomeMoreMenu(current_path);
}

u32 GodMode(int entrypoint) {
    const u32 quick_stp = (MAIN_SCREEN == TOP_SCREEN) ? 20 : 19;
    u32 exit_mode = GODMODE_EXIT_POWEROFF;

    char current_path[256] = { 0x00 }; // don't change this size!
    u32 cursor = 0;
    u32 scroll = 0;

    int mark_next = -1;
    u32 last_write_perm = GetWritePermissions();
    u32 last_clipboard_size = 0;

    bool bootloader = IS_UNLOCKED && (entrypoint == ENTRY_NANDBOOT);
    bool bootmenu = bootloader && (BOOTMENU_KEY != BUTTON_START) && CheckButton(BOOTMENU_KEY);
    bool godmode9 = !bootloader;


    // FIRM from FCRAM handling
    FirmHeader* firm_in_mem = (FirmHeader*) __FIRMTMP_ADDR; // should be safe here
    if (bootloader) { // check for FIRM in FCRAM, but prevent bootloops
        void* addr = (void*) __FIRMRAM_ADDR;
        u32 firm_size = GetFirmSize((FirmHeader*) addr);
        memcpy(firm_in_mem, "その他", 4); // overwrite header to prevent bootloops
        if (firm_size && (firm_size <= (__FIRMRAM_END - __FIRMRAM_ADDR))) {
            memcpy(firm_in_mem, addr, firm_size);
            memcpy(addr, "その他", 4); // to prevent bootloops
        }
    }

    // get mode string for splash screen
    const char* disp_mode = NULL;
    if (bootloader) disp_mode = "bootloaderモード\nR+LEFT　メニュー";
    else if (!IS_UNLOCKED && (entrypoint == ENTRY_NANDBOOT)) disp_mode = "oldloaderモード";
    else if (entrypoint == ENTRY_NTRBOOT) disp_mode = "ntrbootモード";
    else if (entrypoint == ENTRY_UNKNOWN) disp_mode = "不明モード";

    bool show_splash = true;
    #ifdef SALTMODE
    show_splash = !bootloader;
    #endif

    // init font
    if (!SetFont(NULL, 0)) return exit_mode;

    // show splash screen (if enabled)
    ClearScreenF(true, true, COLOR_STD_BG);
    if (show_splash) SplashInit(disp_mode);
    u64 timer = timer_start(); // for splash delay

    InitSDCardFS();
    AutoEmuNandBase(true);
    InitNandCrypto(true); // (entrypoint != ENTRY_B9S);
    InitExtFS();
    if (!CalibrateTouchFromSupportFile())
        CalibrateTouchFromFlash();

    // brightness from file?
    s32 brightness = -1;
    if (LoadSupportFile("gm9bright.cfg", &brightness, 0x4))
        SetScreenBrightness(brightness);

    // custom font handling
    if (CheckSupportFile("font.frf")) {
        u8* riff = (u8*) malloc(0x20000); // arbitrary, should be enough by far
        if (riff) {
            u32 riff_size = LoadSupportFile("font.frf", riff, 0x20000);
            if (riff_size) SetFont(riff, riff_size);
            free(riff);
        }
    } else if (CheckSupportFile("font.pbm")) {
        u8* pbm = (u8*) malloc(0x10000); // arbitrary, should be enough by far
        if (pbm) {
            u32 pbm_size = LoadSupportFile("font.pbm", pbm, 0x10000);
            if (pbm_size) SetFont(pbm, pbm_size);
            free(pbm);
        }
    }

    // check for embedded essential backup
    if (((entrypoint == ENTRY_NANDBOOT) || (entrypoint == ENTRY_B9S)) &&
        !PathExist("S:/essential.exefs") && CheckGenuineNandNcsd() &&
        ShowPrompt(true, "必須ファイルのバックアップが見つかりません。\n作成しますか?")) {
        if (EmbedEssentialBackup("S:/nand.bin") == 0) {
            u32 flags = BUILD_PATH | SKIP_ALL;
            PathCopy(OUTPUT_PATH, "S:/essential.exefs", &flags);
            ShowPrompt(false, "バックアップをSysNANDに組み込み、書き込みを行う。 " OUTPUT_PATH ".");
        }
    }

    // check internal clock
    if (IS_UNLOCKED) { // we could actually do this on any entrypoint
        DsTime dstime;
        get_dstime(&dstime);
        if ((DSTIMEGET(&dstime, bcd_Y) < 18) &&
             ShowPrompt(true, "RTCの日付と時間が\n間違っているようです。 セットしますか?") &&
             ShowRtcSetterPrompt(&dstime, "RTCの日付と時刻を設定する:")) {
            char timestr[32];
            set_dstime(&dstime);
            GetTimeString(timestr, true, true);
            ShowPrompt(false, "新しいRTCの日付と時刻は:\n%s\n \nヒント: ホームメニューの時刻は、\nRTCを設定した後、\n手動で調整する必要があります。", timestr);
        }
    }

    #if defined(SALTMODE)
    show_splash = bootmenu = (bootloader && CheckButton(BOOTMENU_KEY));
    if (show_splash) SplashInit("ソルトモード");
    #else // standard behaviour
    bootmenu = bootmenu || (bootloader && CheckButton(BOOTMENU_KEY)); // second check for boot menu keys
    #endif
    while (CheckButton(BOOTPAUSE_KEY)); // don't continue while these keys are held
    if (show_splash) while (timer_msec( timer ) < 1000); // show splash for at least 1 sec

    // bootmenu handler
    if (bootmenu) {
        bootloader = false;
        while (HID_ReadState() & BUTTON_ANY); // wait until no buttons are pressed
        while (!bootloader && !godmode9) {
            const char* optionstr[6] = { "GodMode9再開", "bootloader再開", "ペイロードを選択...", "スクリプトを選択...",
                "電源を切る", "再起動" };
            int user_select = ShowSelectPrompt(6, optionstr, FLAVOR " bootloaderメニュー.\nアクションを選択:");
            char loadpath[256];
            if (user_select == 1) {
                godmode9 = true;
            } else if (user_select == 2) {
                bootloader = true;
            } else if ((user_select == 3) && (FileSelectorSupport(loadpath, "Bootloaderペイロードメニュー\nペイロードを選択:", PAYLOADS_DIR, "*.firm"))) {
                BootFirmHandler(loadpath, false, false);
            } else if ((user_select == 4) && (FileSelectorSupport(loadpath, "Bootloaderスクリプトメニュー\nスクリプトを選択:", SCRIPTS_DIR, "*.gm9"))) {
                ExecuteGM9Script(loadpath);
            } else if (user_select == 5) {
                exit_mode = GODMODE_EXIT_POWEROFF;
            } else if (user_select == 6) {
                exit_mode = GODMODE_EXIT_REBOOT;
            } else if (user_select) continue;
            break;
        }
    }

    // bootloader handler
    if (bootloader) {
        const char* bootfirm_paths[] = { BOOTFIRM_PATHS };
        if (IsBootableFirm(firm_in_mem, FIRM_MAX_SIZE)) {
            PXI_Barrier(PXI_FIRMLAUNCH_BARRIER);
            BootFirm(firm_in_mem, "sdmc:/bootonce.firm");
        }
        for (u32 i = 0; i < sizeof(bootfirm_paths) / sizeof(char*); i++) {
            BootFirmHandler(bootfirm_paths[i], false, (BOOTFIRM_TEMPS >> i) & 0x1);
        }
        ShowPrompt(false, "起動可能なFIRMが見つかりません。\nGodMode9を再開しますか...");
        godmode9 = true;
    }

    if (godmode9) {
        current_dir = (DirStruct*) malloc(sizeof(DirStruct));
        clipboard = (DirStruct*) malloc(sizeof(DirStruct));
        panedata = (PaneData*) malloc(N_PANES * sizeof(PaneData));
        if (!current_dir || !clipboard || !panedata) {
            ShowPrompt(false, "メモリ不足。"); // just to be safe
            return exit_mode;
        }

        GetDirContents(current_dir, "");
        clipboard->n_entries = 0;
        memset(panedata, 0x00, N_PANES * sizeof(PaneData));
        ClearScreenF(true, true, COLOR_STD_BG); // clear splash
    }

    PaneData* pane = panedata;
    while (godmode9) { // this is the main loop
        // basic sanity checking
        if (!current_dir->n_entries) { // current dir is empty -> revert to root
            ShowPrompt(false, "無効なディレクトリオブジェクト");
            *current_path = '\0';
            SetTitleManagerMode(false);
            DeinitExtFS(); // deinit and...
            InitExtFS(); // reinitialize extended file system
            GetDirContents(current_dir, current_path);
            cursor = 0;
            if (!current_dir->n_entries) { // should not happen, if it does fail gracefully
                ShowPrompt(false, "ルートディレクトリが無効です。");
                return exit_mode;
            }
        }
        if (cursor >= current_dir->n_entries) // cursor beyond allowed range
            cursor = current_dir->n_entries - 1;

        int curr_drvtype = DriveType(current_path);
        DirEntry* curr_entry = &(current_dir->entry[cursor]);
        if ((mark_next >= 0) && (curr_entry->type != T_DOTDOT)) {
            curr_entry->marked = mark_next;
            mark_next = -2;
        }
        DrawDirContents(current_dir, cursor, &scroll);
        DrawUserInterface(current_path, curr_entry, N_PANES ? pane - panedata + 1 : 0);
        DrawTopBar(current_path);

        // check write permissions
        if (~last_write_perm & GetWritePermissions()) {
            if (ShowPrompt(true, "書き込み権限を変更しました。\n再ロックしますか？")) SetWritePermissions(last_write_perm, false);
            last_write_perm = GetWritePermissions();
            continue;
        }

        // handle user input
        u32 pad_state = InputWait(3);
        bool switched = (pad_state & BUTTON_R1);

        // basic navigation commands
        if ((pad_state & BUTTON_A) && (curr_entry->type != T_FILE) && (curr_entry->type != T_DOTDOT)) { // for dirs
            if (switched && !(DriveType(curr_entry->path) & (DRV_SEARCH|DRV_TITLEMAN))) { // exclude Y/Z
                const char* optionstr[8] = { NULL };
                char tpath[16] = { 0 };
                snprintf(tpath, 16, "%2.2s/dbs/title.db", curr_entry->path);
                int n_opt = 0;
                int tman = (!(DriveType(curr_entry->path) & DRV_IMAGE) &&
                    ((strncmp(curr_entry->path, tpath, 16) == 0) ||
                     (!*current_path && PathExist(tpath)))) ? ++n_opt : -1;
                int srch_f = ++n_opt;
                int fixcmac = (!*current_path && ((strspn(curr_entry->path, "14AB") == 1) ||
                    ((GetMountState() == IMG_NAND) && (*(curr_entry->path) == '7')))) ? ++n_opt : -1;
                int dirnfo = ++n_opt;
                int stdcpy = (*current_path && strncmp(current_path, OUTPUT_PATH, 256) != 0) ? ++n_opt : -1;
                int rawdump = (!*current_path && (DriveType(curr_entry->path) & DRV_CART)) ? ++n_opt : -1;
                if (tman > 0) optionstr[tman-1] = "タイトルマネージャーを開く";
                if (srch_f > 0) optionstr[srch_f-1] = "ファイルを検索...";
                if (fixcmac > 0) optionstr[fixcmac-1] = "ドライブ用CMACの修正";
                if (dirnfo > 0) optionstr[dirnfo-1] = (*current_path) ? "ディレクトリ情報を表示する" : "ドライブ情報を表示する";
                if (stdcpy > 0) optionstr[stdcpy-1] = "コピー " OUTPUT_PATH;
                if (rawdump > 0) optionstr[rawdump-1] = "ダンプ " OUTPUT_PATH;
                char namestr[UTF_BUFFER_BYTESIZE(32)];
                TruncateString(namestr, (*current_path) ? curr_entry->path : curr_entry->name, 32, 8);
                int user_select = ShowSelectPrompt(n_opt, optionstr, "%s", namestr);
                if (user_select == tman) {
                    if (InitImgFS(tpath)) {
                        SetTitleManagerMode(true);
                        snprintf(current_path, 256, "Y:");
                        GetDirContents(current_dir, current_path);
                        cursor = 1;
                        scroll = 0;
                    } else ShowPrompt(false, "タイトルマネージャの設定に失敗しました!");
                } else if (user_select == srch_f) {
                    char searchstr[256];
                    snprintf(searchstr, 256, "*");
                    TruncateString(namestr, curr_entry->name, 20, 8);
                    if (ShowKeyboardOrPrompt(searchstr, 256, "検索しますか? %s　\n以下に検索を入力してください。", namestr)) {
                        SetFSSearch(searchstr, curr_entry->path);
                        snprintf(current_path, 256, "Z:");
                        GetDirContents(current_dir, current_path);
                        if (current_dir->n_entries) ShowPrompt(false, " %lu の結果が見つかりました。", current_dir->n_entries - 1);
                        cursor = 1;
                        scroll = 0;
                    }
                } else if (user_select == fixcmac) {
                    RecursiveFixFileCmac(curr_entry->path);
                    ShowPrompt(false, "ドライブ終了時のCMACを修正しました。");
                } else if (user_select == dirnfo) {
                    if (DirFileAttrMenu(curr_entry->path, curr_entry->name)) {
                        ShowPrompt(false, "解析に失敗しました %s\n",
                            (current_path[0] == '\0') ? "ドライブ" : "ディレクトリ"
                        );
                    }
                } else if (user_select == stdcpy) {
                    StandardCopy(&cursor, &scroll);
                } else if (user_select == rawdump) {
                    CartRawDump();
                }
            } else { // one level up
                u32 user_select = 1;
                if (curr_drvtype & DRV_SEARCH) { // special menu for search drive
                    static const char* optionstr[2] = { "このフォルダを開く", "保存しているフォルダを開く" };
                    char pathstr[UTF_BUFFER_BYTESIZE(32)];
                    TruncateString(pathstr, curr_entry->path, 32, 8);
                    user_select = ShowSelectPrompt(2, optionstr, "%s", pathstr);
                }
                if (user_select) {
                    strncpy(current_path, curr_entry->path, 256);
                    current_path[255] = '\0';
                    if (user_select == 2) {
                        char* last_slash = strrchr(current_path, '/');
                        if (last_slash) *last_slash = '\0';
                    }
                    GetDirContents(current_dir, current_path);
                    if (*current_path && (current_dir->n_entries > 1)) {
                        cursor = 1;
                        scroll = 0;
                    } else cursor = 0;
                }
            }
        } else if ((pad_state & BUTTON_A) && (curr_entry->type == T_FILE)) { // process a file
            if (!curr_entry->marked) ShowGameFileIcon(curr_entry->path, ALT_SCREEN);
            DrawTopBar(current_path);
            FileHandlerMenu(current_path, &cursor, &scroll, &pane); // processed externally
            ClearScreenF(true, true, COLOR_STD_BG);
        } else if (*current_path && ((pad_state & BUTTON_B) || // one level down
            ((pad_state & BUTTON_A) && (curr_entry->type == T_DOTDOT)))) {
            if (switched) { // use R+B to return to root fast
                *current_path = '\0';
                GetDirContents(current_dir, current_path);
                cursor = scroll = 0;
            } else {
                char old_path[256];
                char* last_slash = strrchr(current_path, '/');
                strncpy(old_path, current_path, 256);
                if (last_slash) *last_slash = '\0';
                else *current_path = '\0';
                GetDirContents(current_dir, current_path);
                if (*old_path && current_dir->n_entries) {
                    for (cursor = current_dir->n_entries - 1;
                        (cursor > 0) && (strncmp(current_dir->entry[cursor].path, old_path, 256) != 0); cursor--);
                    if (*current_path && !cursor && (current_dir->n_entries > 1)) cursor = 1; // don't set it on the dotdot
                    scroll = 0;
                }
            }
        } else if (switched && (pad_state & BUTTON_B)) { // unmount SD card
            if (!CheckSDMountState()) {
                while (!InitSDCardFS() &&
                    ShowPrompt(true, "SDカードの初期化に失敗しました。再試行しますか？"));
            } else {
                DeinitSDCardFS();
                if (clipboard->n_entries && !PathExist(clipboard->entry[0].path))
                    clipboard->n_entries = 0; // remove SD clipboard entries
            }
            ClearScreenF(true, true, COLOR_STD_BG);
            AutoEmuNandBase(true);
            InitExtFS();
            GetDirContents(current_dir, current_path);
            if (cursor >= current_dir->n_entries) cursor = 0;
        } else if (!switched && (pad_state & BUTTON_DOWN) && (cursor + 1 < current_dir->n_entries))  { // cursor down
            if (pad_state & BUTTON_L1) mark_next = curr_entry->marked;
            cursor++;
        } else if (!switched && (pad_state & BUTTON_UP) && cursor) { // cursor up
            if (pad_state & BUTTON_L1) mark_next = curr_entry->marked;
            cursor--;
        } else if (switched && (pad_state & (BUTTON_RIGHT|BUTTON_LEFT))) { // switch pane
            memcpy(pane->path, current_path, 256);  // store state in current pane
            pane->cursor = cursor;
            pane->scroll = scroll;
            (pad_state & BUTTON_LEFT) ? pane-- : pane++; // switch to next
            if (pane < panedata) pane += N_PANES;
            else if (pane >= panedata + N_PANES) pane -= N_PANES;
            memcpy(current_path, pane->path, 256);  // get state from next pane
            cursor = pane->cursor;
            scroll = pane->scroll;
            GetDirContents(current_dir, current_path);
        } else if (switched && (pad_state & BUTTON_DOWN)) { // force reload file list
            GetDirContents(current_dir, current_path);
            ClearScreenF(true, true, COLOR_STD_BG);
        } else if ((pad_state & BUTTON_RIGHT) && !(pad_state & BUTTON_L1)) { // cursor down (quick)
            cursor += quick_stp;
        } else if ((pad_state & BUTTON_LEFT) && !(pad_state & BUTTON_L1)) { // cursor up (quick)
            cursor = (cursor >= quick_stp) ? cursor - quick_stp : 0;
        } else if ((pad_state & BUTTON_RIGHT) && *current_path) { // mark all entries
            for (u32 c = 1; c < current_dir->n_entries; c++) current_dir->entry[c].marked = 1;
            mark_next = 1;
        } else if ((pad_state & BUTTON_LEFT) && *current_path) { // unmark all entries
            for (u32 c = 1; c < current_dir->n_entries; c++) current_dir->entry[c].marked = 0;
            mark_next = 0;
        } else if (switched && (pad_state & BUTTON_L1)) { // switched L -> screenshot
            // this is handled in hid.h
        } else if (*current_path && (pad_state & BUTTON_L1) && (curr_entry->type != T_DOTDOT)) {
            // unswitched L - mark/unmark single entry
            if (mark_next < -1) mark_next = -1;
            else curr_entry->marked ^= 0x1;
        } else if (pad_state & BUTTON_SELECT) { // clear/restore clipboard
            clipboard->n_entries = (clipboard->n_entries > 0) ? 0 : last_clipboard_size;
        }

        // highly specific commands
        if (!*current_path) { // in the root folder...
            if (switched && (pad_state & BUTTON_X)) { // unmount image
                if (clipboard->n_entries && (DriveType(clipboard->entry[0].path) & DRV_IMAGE))
                    clipboard->n_entries = 0; // remove last mounted image clipboard entries
                SetTitleManagerMode(false);
                InitImgFS(NULL);
                ClearScreenF(false, true, COLOR_STD_BG);
                GetDirContents(current_dir, current_path);
            } else if (switched && (pad_state & BUTTON_Y)) {
                SetWritePermissions(PERM_BASE, false);
                last_write_perm = GetWritePermissions();
                ClearScreenF(false, true, COLOR_STD_BG);
            }
        } else if (!switched) { // standard unswitched command set
            if ((curr_drvtype & DRV_VIRTUAL) && (pad_state & BUTTON_X) && (*current_path != 'T')) {
                ShowPrompt(false, "仮想パスでは不可");
            } else if (pad_state & BUTTON_X) { // delete a file
                u32 n_marked = 0;
                if (curr_entry->marked) {
                    for (u32 c = 0; c < current_dir->n_entries; c++)
                        if (current_dir->entry[c].marked) n_marked++;
                }
                if (n_marked) {
                    if (ShowPrompt(true, "パス %u を削除しますか?", n_marked)) {
                        u32 n_errors = 0;
                        ShowString("ファイルを削除しています、しばらくお待ちください...");
                        for (u32 c = 0; c < current_dir->n_entries; c++)
                            if (current_dir->entry[c].marked && !PathDelete(current_dir->entry[c].path))
                                n_errors++;
                        ClearScreenF(true, false, COLOR_STD_BG);
                        if (n_errors) ShowPrompt(false, " %u/%u パスの削除に失敗しました。", n_errors, n_marked);
                    }
                } else if (curr_entry->type != T_DOTDOT) {
                    char namestr[UTF_BUFFER_BYTESIZE(28)];
                    TruncateString(namestr, curr_entry->name, 28, 12);
                    if (ShowPrompt(true, "削除しますか \"%s\"?", namestr)) {
                        ShowString("ファイルを削除しています、しばらくお待ちください...");
                        if (!PathDelete(curr_entry->path))
                            ShowPrompt(false, "削除の失敗:\n%s", namestr);
                        ClearScreenF(true, false, COLOR_STD_BG);
                    }
                }
                GetDirContents(current_dir, current_path);
            } else if ((pad_state & BUTTON_Y) && (clipboard->n_entries == 0)) { // fill clipboard
                for (u32 c = 0; c < current_dir->n_entries; c++) {
                    if (current_dir->entry[c].marked) {
                        current_dir->entry[c].marked = 0;
                        DirEntryCpy(&(clipboard->entry[clipboard->n_entries]), &(current_dir->entry[c]));
                        clipboard->n_entries++;
                    }
                }
                if ((clipboard->n_entries == 0) && (curr_entry->type != T_DOTDOT)) {
                    DirEntryCpy(&(clipboard->entry[0]), curr_entry);
                    clipboard->n_entries = 1;
                }
                if (clipboard->n_entries)
                    last_clipboard_size = clipboard->n_entries;
            } else if ((curr_drvtype & DRV_SEARCH) && (pad_state & BUTTON_Y)) {
                ShowPrompt(false, "検索ドライブでは不可");
            } else if ((curr_drvtype & DRV_GAME) && (pad_state & BUTTON_Y)) {
                ShowPrompt(false, "仮想ゲームパスでは不可");
            } else if ((curr_drvtype & DRV_XORPAD) && (pad_state & BUTTON_Y)) {
                ShowPrompt(false, "XORpadドライブでは不可");
            } else if ((curr_drvtype & DRV_CART) && (pad_state & BUTTON_Y)) {
                ShowPrompt(false, "ゲームカートドライブでは不可");
            } else if (pad_state & BUTTON_Y) { // paste files
                static const char* optionstr[2] = { "パスをコピー", "パスを移動" };
                char promptstr[64];
                u32 flags = 0;
                u32 user_select;
                if (clipboard->n_entries == 1) {
                    char namestr[UTF_BUFFER_BYTESIZE(20)];
                    TruncateString(namestr, clipboard->entry[0].name, 20, 12);
                    snprintf(promptstr, 64, "ここに \"%s\" 貼り付けますか?", namestr);
                } else snprintf(promptstr, 64, "ここに %lu パスを貼り付けますか?", clipboard->n_entries);
                user_select = ((DriveType(clipboard->entry[0].path) & curr_drvtype & DRV_STDFAT)) ?
                    ShowSelectPrompt(2, optionstr, "%s", promptstr) : (ShowPrompt(true, "%s", promptstr) ? 1 : 0);
                if (user_select) {
                    for (u32 c = 0; c < clipboard->n_entries; c++) {
                        char namestr[UTF_BUFFER_BYTESIZE(36)];
                        TruncateString(namestr, clipboard->entry[c].name, 36, 12);
                        flags &= ~ASK_ALL;
                        if (c < clipboard->n_entries - 1) flags |= ASK_ALL;
                        if ((user_select == 1) && !PathCopy(current_path, clipboard->entry[c].path, &flags)) {
                            if (c + 1 < clipboard->n_entries) {
                                if (!ShowPrompt(true, "パスのコピーに失敗しました:\n%s\nプロセスは残っていますか？", namestr)) break;
                            } else ShowPrompt(false, "パスのコピーに失敗しました:\n%s", namestr);
                        } else if ((user_select == 2) && !PathMove(current_path, clipboard->entry[c].path, &flags)) {
                            if (c + 1 < clipboard->n_entries) {
                                if (!ShowPrompt(true, "パスの移動に失敗しました:\n%s\nプロセスは残っていますか？", namestr)) break;
                            } else ShowPrompt(false, "パスの移動に失敗しました:\n%s", namestr);
                        }
                    }
                    clipboard->n_entries = 0;
                    GetDirContents(current_dir, current_path);
                }
                ClearScreenF(true, false, COLOR_STD_BG);
            }
        } else { // switched command set
            if ((curr_drvtype & DRV_VIRTUAL) && (pad_state & (BUTTON_X|BUTTON_Y))) {
                ShowPrompt(false, "仮想パスでは不可");
            } else if ((curr_drvtype & DRV_ALIAS) && (pad_state & (BUTTON_X))) {
                ShowPrompt(false, "エイリアスパスで使用不可");
            } else if ((pad_state & BUTTON_X) && (curr_entry->type != T_DOTDOT)) { // rename a file
                char newname[256];
                char namestr[UTF_BUFFER_BYTESIZE(20)];
                TruncateString(namestr, curr_entry->name, 20, 12);
                snprintf(newname, 255, "%s", curr_entry->name);
                if (ShowKeyboardOrPrompt(newname, 256, "名前を変更しますか %s?\n新しい名前を入力してください。", namestr)) {
                    if (!PathRename(curr_entry->path, newname))
                        ShowPrompt(false, "パス名の変更に失敗しました:\n%s", namestr);
                    else {
                        GetDirContents(current_dir, current_path);
                        for (cursor = (current_dir->n_entries) ? current_dir->n_entries - 1 : 0;
                            (cursor > 1) && (strncmp(current_dir->entry[cursor].name, newname, 256) != 0); cursor--);
                    }
                }
            } else if (pad_state & BUTTON_Y) { // create an entry
                static const char* optionstr[] = { "フォルダの作成", "ダミーファイルの作成" };
                u32 type = ShowSelectPrompt(2, optionstr, "ここに新しいエントリーを作成しますか？\nタイプを選択。");
                if (type) {
                    const char* typestr = (type == 1) ? "フォルダ" : (type == 2) ? "ファイル" : NULL;
                    char ename[256];
                    u64 fsize = 0;
                    snprintf(ename, 255, (type == 1) ? "新規ディレクトリ" : "dummy.bin");
                    if ((ShowKeyboardOrPrompt(ename, 256, "ここで新しい %s を作成しますか？\n以下に名前を入力してください。", typestr)) &&
                        ((type != 2) || ((fsize = ShowNumberPrompt(0, "ここで新しい %s を作成しますか？\nファイルサイズを下に入力してください。", typestr)) != (u64) -1))) {
                        if (((type == 1) && !DirCreate(current_path, ename)) ||
                            ((type == 2) && !FileCreateDummy(current_path, ename, fsize))) {
                            char namestr[UTF_BUFFER_BYTESIZE(36)];
                            TruncateString(namestr, ename, 36, 12);
                            ShowPrompt(false, "作成に失敗しました %s:\n%s", typestr, namestr);
                        } else {
                            GetDirContents(current_dir, current_path);
                            for (cursor = (current_dir->n_entries) ? current_dir->n_entries - 1 : 0;
                                (cursor > 1) && (strncmp(current_dir->entry[cursor].name, ename, 256) != 0); cursor--);
                        }
                    }
                }
            }
        }

        if (pad_state & BUTTON_START) {
            exit_mode = (switched || (pad_state & BUTTON_LEFT)) ? GODMODE_EXIT_POWEROFF : GODMODE_EXIT_REBOOT;
            break;
        } else if (pad_state & (BUTTON_HOME|BUTTON_POWER)) { // Home menu
            const char* optionstr[8];
            const char* buttonstr = (pad_state & BUTTON_HOME) ? "HOME" : "電源";
            u32 n_opt = 0;
            int poweroff = ++n_opt;
            int reboot = ++n_opt;
            int brick = (HID_ReadState() & BUTTON_R1) ? ++n_opt : 0;
            int titleman = ++n_opt;
            int scripts = ++n_opt;
            int payloads = ++n_opt;
            int more = ++n_opt;
            if (poweroff > 0) optionstr[poweroff - 1] = "シャットダウン";
            if (reboot > 0) optionstr[reboot - 1] = "再起動";
            if (titleman > 0) optionstr[titleman - 1] = "タイトルマネージャー";
            if (brick > 0) optionstr[brick - 1] = "3DSを壊す";
            if (scripts > 0) optionstr[scripts - 1] = "スクリプト...";
            if (payloads > 0) optionstr[payloads - 1] = "ペイロード...";
            if (more > 0) optionstr[more - 1] = "その他...";

            int user_select = 0;
            while ((user_select = ShowSelectPrompt(n_opt, optionstr, "%s ボタンが押されました。\nアクションを選択:", buttonstr)) &&
                (user_select != poweroff) && (user_select != reboot)) {
                char loadpath[256];
                if ((user_select == more) && (HomeMoreMenu(current_path) == 0)) break; // more... menu
                else if (user_select == titleman) {
                    static const char* tmoptionstr[4] = {
                        "[A:] SDカード",
                        "[1:] NAND / TWL",
                        "[B:] SDカード",
                        "[4:] NAND / TWL"
                    };
                    static const char* tmpaths[4] = {
                        "A:/dbs/title.db",
                        "1:/dbs/title.db",
                        "B:/dbs/title.db",
                        "4:/dbs/title.db"
                    };
                    u32 tmnum = 2;
                    if (!CheckSDMountState() || (tmnum = ShowSelectPrompt(
                        (CheckVirtualDrive("E:")) ? 4 : 2, tmoptionstr,
                        "タイトルマネージャーメニュー\nタイトルソースの選択:", buttonstr))) {
                        const char* tpath = tmpaths[tmnum-1];
                        if (InitImgFS(tpath)) {
                            SetTitleManagerMode(true);
                            snprintf(current_path, 256, "Y:");
                            GetDirContents(current_dir, current_path);
                            ClearScreenF(true, true, COLOR_STD_BG);
                            cursor = 1;
                            scroll = 0;
                            break;
                        } else ShowPrompt(false, "タイトルマネージャの設定に失敗しました!");
                    }
                } else if (user_select == scripts) {
                    if (!CheckSupportDir(SCRIPTS_DIR)) {
                        ShowPrompt(false, "Scriptsディレクトリが見つかりません。\n(default path: 0:/gm9/" SCRIPTS_DIR ")");
                    } else if (FileSelectorSupport(loadpath, "HOME スクリプト... メニュー.\nスクリプトを選択:", SCRIPTS_DIR, "*.gm9")) {
                        ExecuteGM9Script(loadpath);
                        GetDirContents(current_dir, current_path);
                        ClearScreenF(true, true, COLOR_STD_BG);
                        break;
                    }
                } else if (user_select == payloads) {
                    if (!CheckSupportDir(PAYLOADS_DIR)) ShowPrompt(false, "Payloadsディレクトリが見つかりません。\n(デフォルトパス: 0:/gm9/" PAYLOADS_DIR ")");
                    else if (FileSelectorSupport(loadpath, "HOME ペイロード... メニュー.\nペイロードを選択:", PAYLOADS_DIR, "*.firm"))
                        BootFirmHandler(loadpath, false, false);
                } else if (user_select == brick) {
                    Paint9(); // hiding a secret here
                    ClearScreenF(true, true, COLOR_STD_BG);
                    break;
                }
            }

            if (user_select == poweroff) {
                exit_mode = GODMODE_EXIT_POWEROFF;
                break;
            } else if (user_select == reboot) {
                exit_mode = GODMODE_EXIT_REBOOT;
                break;
            }
        } else if (pad_state & (CART_INSERT|CART_EJECT)) {
            if (!InitVCartDrive() && (pad_state & CART_INSERT) &&
                (curr_drvtype & DRV_CART)) // reinit virtual cart drive
                ShowPrompt(false, "カートの起動に失敗しました!");
            if (!(*current_path) || (curr_drvtype & DRV_CART))
                GetDirContents(current_dir, current_path); // refresh dir contents
        } else if (pad_state & SD_INSERT) {
            while (!InitSDCardFS() && ShowPrompt(true, "SDカードの初期化に失敗しました。再試行しますか？"));
            ClearScreenF(true, true, COLOR_STD_BG);
            AutoEmuNandBase(true);
            InitExtFS();
            GetDirContents(current_dir, current_path);
        } else if ((pad_state & SD_EJECT) && CheckSDMountState()) {
            ShowPrompt(false, "!SDカードの予期せぬ取り外し!\n \nデータの損失を防ぐため、SDカードを取り出す前に\nアンマウントしてください。");
            DeinitExtFS();
            DeinitSDCardFS();
            InitExtFS();
            if (clipboard->n_entries && (DriveType(clipboard->entry[0].path) &
                (DRV_SDCARD|DRV_ALIAS|DRV_EMUNAND|DRV_IMAGE)))
                clipboard->n_entries = 0; // remove SD clipboard entries
            GetDirContents(current_dir, current_path);
        }
    }


    DeinitExtFS();
    DeinitSDCardFS();

    if (current_dir) free(current_dir);
    if (clipboard) free(clipboard);
    if (panedata) free(panedata);

    return exit_mode;
}

#else
u32 ScriptRunner(int entrypoint) {
    // init font and show splash
    if (!SetFont(NULL, 0)) return GODMODE_EXIT_POWEROFF;
    SplashInit("scriptrunnerモード");
    u64 timer = timer_start();

    InitSDCardFS();
    AutoEmuNandBase(true);
    InitNandCrypto(entrypoint != ENTRY_B9S);
    InitExtFS();
    if (!CalibrateTouchFromSupportFile())
        CalibrateTouchFromFlash();

    // brightness from file?
    s32 brightness = -1;
    if (LoadSupportFile("gm9bright.cfg", &brightness, 0x4))
        SetScreenBrightness(brightness);

    while (CheckButton(BOOTPAUSE_KEY)); // don't continue while these keys are held
    while (timer_msec( timer ) < 500); // show splash for at least 0.5 sec

    // you didn't really install a scriptrunner to NAND, did you?
    if (IS_UNLOCKED && (entrypoint == ENTRY_NANDBOOT))
        BootFirmHandler("0:/iderped.firm", false, false);

    if (PathExist("V:/" VRAM0_AUTORUN_GM9)) {
        ClearScreenF(true, true, COLOR_STD_BG); // clear splash
        ExecuteGM9Script("V:/" VRAM0_AUTORUN_GM9);
    } else if (PathExist("V:/" VRAM0_SCRIPTS)) {
        char loadpath[256];
        if (FileSelector(loadpath, FLAVOR " スクリプトメニュー\nスクリプトを選択:", "V:/" VRAM0_SCRIPTS, "*.gm9", HIDE_EXT, false))
            ExecuteGM9Script(loadpath);
    } else ShowPrompt(false, "スクリプトがコンパイルされましたが、\n　スクリプトが提供されていない。　\n \nDerp!");

    // deinit
    DeinitExtFS();
    DeinitSDCardFS();

    return GODMODE_EXIT_REBOOT;
}
#endif
