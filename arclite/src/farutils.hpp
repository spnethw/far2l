﻿#pragma once

extern std::wstring get_error_dlg_title();

#define FAR_ERROR_HANDLER_BEGIN                                                                              \
	try {                                                                                                    \
		try {

#define FAR_ERROR_HANDLER_END(return_error, return_cancel, silent)                                           \
	}                                                                                                        \
	catch (const Error &e)                                                                                   \
	{                                                                                                        \
		if (e.code == E_ABORT) {                                                                             \
			return_cancel;                                                                                   \
		} else {                                                                                             \
			if (!(silent))                                                                                   \
				Far::error_dlg(get_error_dlg_title(), e);                                                    \
			return_error;                                                                                    \
		}                                                                                                    \
	}                                                                                                        \
	catch (const std::exception &e)                                                                          \
	{                                                                                                        \
		if (!(silent))                                                                                       \
			Far::error_dlg(get_error_dlg_title(), e);                                                        \
		return_error;                                                                                        \
	}                                                                                                        \
	}                                                                                                        \
	catch (...)                                                                                              \
	{                                                                                                        \
		return_error;                                                                                        \
	}

namespace Far
{

extern FarStandardFunctions g_fsf;

void init(const PluginStartupInfo *psi);
std::wstring get_plugin_module_path();

const wchar_t *msg_ptr(int id);
std::wstring get_msg(int id);

unsigned get_optimal_msg_width();
//intptr_t message(const GUID &id, const std::wstring &msg, int button_cnt = 0, FARMESSAGEFLAGS flags = 0);
intptr_t message(const GUID &id, const std::wstring &msg, int button_cnt = 0, FARMESSAGEFLAGS flags = FMSG_NONE);


class MenuItems : public std::vector<std::wstring>
{
public:
	unsigned add(const std::wstring &item);
};
intptr_t menu(const GUID &id, const std::wstring &title, const MenuItems &items, const wchar_t *help = {});

std::wstring get_progress_bar_str(unsigned width, UInt64 completed, UInt64 total);
void set_progress_state(PROGRESSTATE state);
void set_progress_value(UInt64 completed, UInt64 total);
void progress_notify();

void call_user_apc(void *param);
bool post_macro(const std::wstring &macro);
void quit();

HANDLE save_screen();
void restore_screen(HANDLE h_scr);
void flush_screen();

intptr_t viewer(const std::wstring &file_name, const std::wstring &title, uint32_t flags = 0);
intptr_t editor(const std::wstring &file_name, const std::wstring &title, uint32_t flags = 0);

void update_panel(HANDLE h_panel, const bool keep_selection, const bool reset_position = false);
void set_view_mode(HANDLE h_panel, unsigned view_mode);
void set_sort_mode(HANDLE h_panel, unsigned sort_mode);
void set_reverse_sort(HANDLE h_panel, bool reverse_sort);
void set_directories_first(HANDLE h_panel, bool first);

// bool get_panel_info(HANDLE h_panel, PanelInfo &panel_info);
bool get_panel_info(HANDLE h_panel, PanelInfo &panel_info);

bool is_real_file_panel(const PanelInfo &panel_info);
std::wstring get_panel_dir(HANDLE h_panel);

void get_panel_item(HANDLE h_panel, uint32_t command, size_t index, Buffer<unsigned char> &buf);
struct PanelItem
{
	uintptr_t file_attributes;
	FILETIME creation_time;
	FILETIME last_access_time;
	FILETIME last_write_time;
	UInt64 file_size;
	UInt64 pack_size;
	std::wstring file_name;
	std::wstring alt_file_name;
	void *user_data;
};
PanelItem get_current_panel_item(HANDLE h_panel);
PanelItem get_panel_item(HANDLE h_panel, size_t index);
PanelItem get_selected_panel_item(HANDLE h_panel, size_t index);

void error_dlg(const std::wstring &title, const Error &e);
void info_dlg(const GUID &id, const std::wstring &title, const std::wstring &msg);
bool input_dlg(const GUID &id, const std::wstring &title, const std::wstring &msg, std::wstring &text,
		INPUTBOXFLAGS flags = FIB_NONE);

#define AUTO_SIZE ((intptr_t)-1)
const unsigned c_x_frame = 5;
const unsigned c_y_frame = 2;

struct DialogItem
{
	uint32_t type{};
	size_t x1{};
	size_t y1{};
	size_t x2{};
	size_t y2{};
	uint32_t flags{};
	int selected{};
	int def_button{};
	unsigned history_idx{};
	unsigned mask_idx{};
	unsigned text_idx{};
	unsigned list_idx{};
	size_t list_size{};
	size_t list_pos{};
};

class Dialog
{
private:
	size_t client_xs;
	size_t client_ys{};
	size_t x;
	size_t y;
	const wchar_t *help;
	uint32_t m_flags;
	std::vector<std::wstring> values;
	std::vector<DialogItem> items;
	HANDLE h_dlg{};
	const GUID *guid;
	unsigned new_value(const std::wstring &text);
	const wchar_t *get_value(unsigned idx) const;
	void frame(const std::wstring &text);
	void calc_frame_size();
	unsigned new_item(const DialogItem &di);
	static intptr_t WINAPI internal_dialog_proc(HANDLE h_dlg, int msg, int param1, void *param2);
	bool events_enabled;

protected:
	class DisableEvents
	{
	private:
		Dialog &dlg;
		bool events_enabled;

	public:
		DisableEvents(Dialog &dlg) : dlg(dlg), events_enabled(dlg.events_enabled)
		{
			dlg.events_enabled = false;
			dlg.send_message(DM_ENABLEREDRAW, FALSE);
		}
		~DisableEvents()
		{
			dlg.send_message(DM_ENABLEREDRAW, TRUE);
			dlg.events_enabled = events_enabled;
		}
	};
	size_t get_label_len(const std::wstring &str, uint32_t flags);
	intptr_t default_dialog_proc(intptr_t msg, intptr_t param1, void *param2);
	virtual intptr_t dialog_proc(intptr_t msg, intptr_t param1, void *param2)
	{
		return default_dialog_proc(msg, param1, param2);
	}
	void set_width(unsigned width) { client_xs = width; }
	intptr_t send_message(intptr_t msg, intptr_t param1, void *param2 = nullptr);

public:
	Dialog(const std::wstring &title, const GUID *guid, unsigned width = 60, const wchar_t *help = nullptr,
			uint32_t flags = 0);
	virtual ~Dialog() = default;
	// create different controls
	void new_line();
	void reset_line();
	void spacer(size_t size);
	void pad(size_t pos);
	unsigned separator();
	unsigned separator(const std::wstring &text);
	unsigned label(const std::wstring &text, size_t boxsize = AUTO_SIZE, uint32_t flags = 0);
	unsigned edit_box(const std::wstring &text, size_t boxsize = AUTO_SIZE, uint32_t flags = 0);
	unsigned mask_edit_box(const std::wstring &text, const std::wstring &mask, size_t boxsize = AUTO_SIZE,
			uint32_t flags = 0);
	unsigned history_edit_box(const std::wstring &text, const std::wstring &history_name,
			size_t boxsize = AUTO_SIZE, uint32_t flags = 0);
	unsigned fix_edit_box(const std::wstring &text, size_t boxsize = AUTO_SIZE, uint32_t flags = 0);
	unsigned pwd_edit_box(const std::wstring &text, size_t boxsize = AUTO_SIZE, uint32_t flags = 0);
	unsigned button(const std::wstring &text, uint32_t flags = 0);
	unsigned def_button(const std::wstring &text, uint32_t flags = 0)
	{
		return button(text, flags | DIF_DEFAULT);
	}
	unsigned check_box(const std::wstring &text, int value, uint32_t flags = 0);
	unsigned check_box(const std::wstring &text, bool value, uint32_t flags = 0)
	{
		return check_box(text, value ? BSTATE_CHECKED : BSTATE_UNCHECKED, flags);
	}
	unsigned check_box3(const std::wstring &text, bool value, bool value_defined, uint32_t flags = 0)
	{
		return check_box(text, value_defined ? (value ? BSTATE_CHECKED : BSTATE_UNCHECKED) : BSTATE_3STATE,
				flags | DIF_3STATE);
	}
	unsigned check_box3(const std::wstring &text, TriState value, uint32_t flags = 0)
	{
		return check_box(text,
				value == triUndef ? BSTATE_3STATE
						: value == triTrue
						? BSTATE_CHECKED
						: BSTATE_UNCHECKED,
				flags | DIF_3STATE);
	}
	unsigned radio_button(const std::wstring &text, bool value, uint32_t flags = 0);
	unsigned combo_box(const std::vector<std::wstring> &items, size_t sel_idx, size_t boxsize = AUTO_SIZE,
			uint32_t flags = 0);
	// display dialog
	intptr_t show();
	// utilities to set/get control values
	std::wstring get_text(unsigned ctrl_id) const;
	size_t get_text(unsigned ctrl_id, wchar_t *buf, size_t bufsize);

	void set_text(unsigned ctrl_id, const std::wstring &text);
	void set_text(unsigned ctrl_id, const wchar_t *text);
	void set_text_silent(unsigned ctrl_id, const std::wstring &text);
	void set_text_silent(unsigned ctrl_id, const wchar_t *text);
	bool get_check(unsigned ctrl_id) const;
	void set_check(unsigned ctrl_id, bool check = true);
	TriState get_check3(unsigned ctrl_id) const;
	void set_check3(unsigned ctrl_id, TriState check);
	unsigned get_list_pos(unsigned ctrl_id) const;
	void set_list_pos(unsigned ctrl_id, uintptr_t pos);

	void set_focus(unsigned ctrl_id);
	void enable(unsigned ctrl_id, bool enable);
	void set_visible(unsigned ctrl_id, bool visible);
};

class Regex : private NonCopyable
{
private:
	HANDLE h_regex;

public:
	Regex();
	~Regex();
	size_t search(const std::wstring &expr, const std::wstring &text);
};

class Selection
{
private:
	HANDLE h_plugin;

public:
	Selection(HANDLE h_plugin);
	~Selection();
	void select(unsigned idx, bool value);
};

class FileFilter : private NonCopyable
{
private:
	HANDLE h_filter;
	void clean();

public:
	FileFilter();
	~FileFilter();
	bool create(HANDLE h_panel, int type);
	bool menu();
	void start();
	bool match(const PluginPanelItem &panel_item);
};

std::wstring get_absolute_path(const std::wstring &rel_path);
INT_PTR control(HANDLE h_panel, FILE_CONTROL_COMMANDS command, int param1 = 0, void *param2 = nullptr);
INT_PTR adv_control(ADVANCED_CONTROL_COMMANDS command, int param1 = 0, void *param2 = nullptr);
//INT_PTR adv_control_async(ADVANCED_CONTROL_COMMANDS command, int param1, void *param2 = nullptr);
bool match_masks(const std::wstring &file_name, const std::wstring &masks);
bool get_color(PaletteColors color_id, UInt64 &color);
bool panel_go_to_dir(HANDLE h_panel, const std::wstring &dir);
bool panel_go_to_file(HANDLE h_panel, const std::wstring &file_path);
void panel_go_to_part(HANDLE h_panek, const int part_idx);
DWORD get_lang_id();
void close_panel(HANDLE h_panel, const std::wstring &dir);
void open_help(const std::wstring &topic);

class Settings
{
private:
	std::unique_ptr<KeyFileHelper> _settings_kfh;
	std::string settingsIni;
	std::string cSectionName;
	//  HANDLE handle;
	size_t dir_id;
	INT_PTR control(uint32_t command, void *param = nullptr);
	void clean();

public:
	Settings();
	~Settings();
	bool create(bool app_settings = false);
	bool set_dir(const std::wstring &path);
	bool have_dir(const std::wstring &path);
	bool list_dir(std::vector<std::wstring> &result);
	bool set(const wchar_t *name, int value);
	bool set(const wchar_t *name, unsigned int value);
	bool set(const wchar_t *name, UInt64 value);
	bool set(const wchar_t *name, const std::wstring &value);
	bool set(const wchar_t *name, const void *value, size_t value_size);
	bool get(const wchar_t *name, int &value);
	bool get(const wchar_t *name, unsigned int &value);
	bool get(const wchar_t *name, UInt64 &value);
	bool get(const wchar_t *name, std::wstring &value);
	bool get(const wchar_t *name, ByteVector &value);
	bool get(const wchar_t *dirname, const wchar_t *name, UInt64 &value);
	bool get(const wchar_t *dirname, const wchar_t *name, std::wstring &value);
	bool get(const wchar_t *dirname, const wchar_t *name, ByteVector &value);
	bool del(const wchar_t *name);
	bool del_dir(const wchar_t *name);
};

// const ArclitePrivateInfo* get_system_functions();

};	  // namespace Far
