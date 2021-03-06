#include "stdafx.h"
#include "Utilities/Config.h"
#include "Utilities/AutoPause.h"
#include "Utilities/event.h"
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"

#include "Emu/Cell/PPUThread.h"
#include "Emu/Cell/PPUCallback.h"
#include "Emu/Cell/PPUOpcodes.h"
#include "Emu/Cell/SPUThread.h"
#include "Emu/Cell/RawSPUThread.h"
#include "Emu/Cell/lv2/sys_sync.h"
#include "Emu/PSP2/ARMv7Thread.h"

#include "Emu/IdManager.h"
#include "Emu/RSX/GSRender.h"

#include "Loader/PSF.h"
#include "Loader/ELF.h"

#include "Utilities/StrUtil.h"

#include "../Crypto/unself.h"

#include <thread>

system_type g_system;

cfg::bool_entry g_cfg_autostart(cfg::root.misc, "Always start after boot", true);
cfg::bool_entry g_cfg_autoexit(cfg::root.misc, "Exit RPCS3 when process finishes");

cfg::string_entry g_cfg_vfs_emulator_dir(cfg::root.vfs, "$(EmulatorDir)"); // Default (empty): taken from fs::get_config_dir()
cfg::string_entry g_cfg_vfs_dev_hdd0(cfg::root.vfs, "/dev_hdd0/", "$(EmulatorDir)dev_hdd0/");
cfg::string_entry g_cfg_vfs_dev_hdd1(cfg::root.vfs, "/dev_hdd1/", "$(EmulatorDir)dev_hdd1/");
cfg::string_entry g_cfg_vfs_dev_flash(cfg::root.vfs, "/dev_flash/", "$(EmulatorDir)dev_flash/");
cfg::string_entry g_cfg_vfs_dev_usb000(cfg::root.vfs, "/dev_usb000/", "$(EmulatorDir)dev_usb000/");
cfg::string_entry g_cfg_vfs_dev_bdvd(cfg::root.vfs, "/dev_bdvd/"); // Not mounted
cfg::string_entry g_cfg_vfs_app_home(cfg::root.vfs, "/app_home/"); // Not mounted

cfg::bool_entry g_cfg_vfs_allow_host_root(cfg::root.vfs, "Enable /host_root/", true);

std::string g_cfg_defaults;

extern atomic_t<u32> g_thread_count;

extern u64 get_system_time();

extern void ppu_load_exec(const ppu_exec_object&);
extern void spu_load_exec(const spu_exec_object&);
extern void arm_load_exec(const arm_exec_object&);
extern std::shared_ptr<struct lv2_prx> ppu_load_prx(const ppu_prx_object&, const std::string&);
extern void ppu_finalize();

fs::file g_tty;

namespace rpcs3
{
	event<void>& on_run() { static event<void> on_run; return on_run; }
	event<void>& on_stop() { static event<void> on_stop; return on_stop; }
	event<void>& on_pause() { static event<void> on_pause; return on_pause; }
	event<void>& on_resume() { static event<void> on_resume; return on_resume; }
}

Emulator::Emulator()
	: m_status(Stopped)
	, m_cpu_thr_stop(0)
{
}

void Emulator::Init()
{
	if (!g_tty)
	{
		g_tty.open(fs::get_config_dir() + "TTY.log", fs::rewrite + fs::append);
	}
	
	idm::init();
	fxm::init();

	// Reset defaults, cache them
	cfg::root.from_default();
	g_cfg_defaults = cfg::root.to_string();

	// Reload global configuration
	cfg::root.from_string(fs::file(fs::get_config_dir() + "/config.yml", fs::read + fs::create).to_string());

	// Create directories
	const std::string emu_dir_ = g_cfg_vfs_emulator_dir;
	const std::string emu_dir = emu_dir_.empty() ? fs::get_config_dir() : emu_dir_;
	const std::string dev_hdd0 = fmt::replace_all(g_cfg_vfs_dev_hdd0, "$(EmulatorDir)", emu_dir);
	const std::string dev_hdd1 = fmt::replace_all(g_cfg_vfs_dev_hdd1, "$(EmulatorDir)", emu_dir);
	const std::string dev_usb = fmt::replace_all(g_cfg_vfs_dev_usb000, "$(EmulatorDir)", emu_dir);

	fs::create_path(dev_hdd0);
	fs::create_dir(dev_hdd0 + "game/");
	fs::create_dir(dev_hdd0 + "game/TEST12345/");
	fs::create_dir(dev_hdd0 + "game/TEST12345/USRDIR/");
	fs::create_dir(dev_hdd0 + "home/");
	fs::create_dir(dev_hdd0 + "home/00000001/");
	fs::create_dir(dev_hdd0 + "home/00000001/exdata/");
	fs::create_dir(dev_hdd0 + "home/00000001/savedata/");
	fs::create_dir(dev_hdd0 + "home/00000001/trophy/");
	if (fs::file f{dev_hdd0 + "home/00000001/localusername", fs::create + fs::excl + fs::write}) f.write("User"s);
	fs::create_dir(dev_hdd1 + "cache/");
	fs::create_dir(dev_hdd1 + "game/");
	fs::create_path(dev_hdd1);
	fs::create_path(dev_usb);

	SetCPUThreadStop(0);
}

void Emulator::SetPath(const std::string& path, const std::string& elf_path)
{
	m_path = path;
	m_elf_path = elf_path;
}

bool Emulator::BootGame(const std::string& path, bool direct)
{
	static const char* boot_list[] =
	{
		"/PS3_GAME/USRDIR/EBOOT.BIN",
		"/USRDIR/EBOOT.BIN",
		"/EBOOT.BIN",
		"/eboot.bin",
	};

	if (direct && fs::is_file(path))
	{
		SetPath(path);
		Load();

		return true;
	}

	for (std::string elf : boot_list)
	{
		elf = path + elf;

		if (fs::is_file(elf))
		{
			SetPath(elf);
			Load();

			return true;
		}
	}

	return false;
}

std::string Emulator::GetGameDir()
{
	const std::string& emu_dir_ = g_cfg_vfs_emulator_dir;
	const std::string& emu_dir = emu_dir_.empty() ? fs::get_config_dir() : emu_dir_;

	return fmt::replace_all(g_cfg_vfs_dev_hdd0, "$(EmulatorDir)", emu_dir) + "game/";
}

std::string Emulator::GetLibDir()
{
	const std::string& emu_dir_ = g_cfg_vfs_emulator_dir;
	const std::string& emu_dir = emu_dir_.empty() ? fs::get_config_dir() : emu_dir_;

	return fmt::replace_all(g_cfg_vfs_dev_flash, "$(EmulatorDir)", emu_dir) + "sys/external/";
}

void Emulator::Load()
{
	Stop();

	try
	{
		Init();

		// Open SELF or ELF
		fs::file elf_file(m_path);

		if (!elf_file)
		{
			LOG_ERROR(LOADER, "Failed to open file: %s", m_path);
			return;
		}

		LOG_NOTICE(LOADER, "Path: %s", m_path);

		const std::string elf_dir = fs::get_parent_dir(m_path);
		const fs::file sfov(elf_dir + "/sce_sys/param.sfo");
		const fs::file sfo1(elf_dir + "/../PARAM.SFO");

		// Load PARAM.SFO (TODO)
		const auto _psf = psf::load_object(sfov ? sfov : sfo1);
		m_title = psf::get_string(_psf, "TITLE", m_path);
		m_title_id = psf::get_string(_psf, "TITLE_ID");

		LOG_NOTICE(LOADER, "Title: %s", GetTitle());
		LOG_NOTICE(LOADER, "Serial: %s", GetTitleID());

		// Initialize data/cache directory
		m_cache_path = fs::get_data_dir(m_title_id, m_path);
		LOG_NOTICE(LOADER, "Cache: %s", GetCachePath());

		// Load custom config-0
		if (fs::file cfg_file{m_cache_path + "/config.yml"})
		{
			LOG_NOTICE(LOADER, "Applying custom config: %s/config.yml", m_cache_path);
			cfg::root.from_string(cfg_file.to_string());
		}

		// Load custom config-1
		if (fs::file cfg_file{fs::get_config_dir() + "data/" + m_title_id + "/config.yml"})
		{
			LOG_NOTICE(LOADER, "Applying custom config: data/%s/config.yml", m_title_id);
			cfg::root.from_string(cfg_file.to_string());
		}

		// Load custom config-2
		if (fs::file cfg_file{m_path + ".yml"})
		{
			LOG_NOTICE(LOADER, "Applying custom config: %s.yml", m_path);
			cfg::root.from_string(cfg_file.to_string());
		}

		LOG_NOTICE(LOADER, "Used configuration:\n%s\n", cfg::root.to_string());

		// Mount all devices
		const std::string emu_dir_ = g_cfg_vfs_emulator_dir;
		const std::string emu_dir = emu_dir_.empty() ? fs::get_config_dir() : emu_dir_;
		const std::string bdvd_dir = g_cfg_vfs_dev_bdvd;
		const std::string home_dir = g_cfg_vfs_app_home;

		vfs::mount("dev_hdd0", fmt::replace_all(g_cfg_vfs_dev_hdd0, "$(EmulatorDir)", emu_dir));
		vfs::mount("dev_hdd1", fmt::replace_all(g_cfg_vfs_dev_hdd1, "$(EmulatorDir)", emu_dir));
		vfs::mount("dev_flash", fmt::replace_all(g_cfg_vfs_dev_flash, "$(EmulatorDir)", emu_dir));
		vfs::mount("dev_usb", fmt::replace_all(g_cfg_vfs_dev_usb000, "$(EmulatorDir)", emu_dir));
		vfs::mount("dev_usb000", fmt::replace_all(g_cfg_vfs_dev_usb000, "$(EmulatorDir)", emu_dir));
		vfs::mount("app_home", home_dir.empty() ? elf_dir + '/' : fmt::replace_all(home_dir, "$(EmulatorDir)", emu_dir));

		// Mount /dev_bdvd/ if necessary
		if (bdvd_dir.empty() && fs::is_file(elf_dir + "/../../PS3_DISC.SFB"))
		{
			const auto dir_list = fmt::split(elf_dir, { "/", "\\" });

			// Check latest two directories
			if (dir_list.size() >= 2 && dir_list.back() == "USRDIR" && *(dir_list.end() - 2) == "PS3_GAME")
			{
				vfs::mount("dev_bdvd", elf_dir.substr(0, elf_dir.length() - 15));
			}
			else
			{
				vfs::mount("dev_bdvd", elf_dir + "/../../");
			}

			LOG_NOTICE(LOADER, "Disc: %s", vfs::get("/dev_bdvd"));
		}
		else if (bdvd_dir.size())
		{
			vfs::mount("dev_bdvd", fmt::replace_all(bdvd_dir, "$(EmulatorDir)", emu_dir));
		}

		// Mount /host_root/ if necessary
		if (g_cfg_vfs_allow_host_root)
		{
			vfs::mount("host_root", {});
		}

		// Check SELF header
		if (elf_file.size() >= 4 && elf_file.read<u32>() == "SCE\0"_u32)
		{
			const std::string decrypted_path = m_cache_path + "boot.elf";

			fs::stat_t encrypted_stat = elf_file.stat();
			fs::stat_t decrypted_stat;

			// Check modification time and try to load decrypted ELF
			if (fs::stat(decrypted_path, decrypted_stat) && decrypted_stat.mtime == encrypted_stat.mtime)
			{
				elf_file.open(decrypted_path);
			}
			else
			{
				// Decrypt SELF
				elf_file = decrypt_self(std::move(elf_file));

				if (fs::file elf_out{decrypted_path, fs::rewrite})
				{
					elf_out.write(elf_file.to_vector<u8>());
					elf_out.close();
					fs::utime(decrypted_path, encrypted_stat.atime, encrypted_stat.mtime);
				}
				else
				{
					LOG_ERROR(LOADER, "Failed to create boot.elf");
				}
			}
		}

		ppu_exec_object ppu_exec;
		ppu_prx_object ppu_prx;
		spu_exec_object spu_exec;
		arm_exec_object arm_exec;

		if (!elf_file)
		{
			LOG_ERROR(LOADER, "Failed to decrypt SELF: %s", m_path);
			return;
		}
		else if (ppu_exec.open(elf_file) == elf_error::ok)
		{
			// PS3 executable
			g_system = system_type::ps3;
			m_status = Ready;
			vm::ps3::init();

			if (m_elf_path.empty())
			{
				m_elf_path = "/host_root/" + m_path;
				LOG_NOTICE(LOADER, "Elf path: %s", m_elf_path);
			}

			ppu_load_exec(ppu_exec);

			fxm::import<GSRender>(Emu.GetCallbacks().get_gs_render); // TODO: must be created in appropriate sys_rsx syscall
		}
		else if (ppu_prx.open(elf_file) == elf_error::ok)
		{
			// PPU PRX (experimental)
			g_system = system_type::ps3;
			m_status = Ready;
			vm::ps3::init();
			ppu_load_prx(ppu_prx, "");
		}
		else if (spu_exec.open(elf_file) == elf_error::ok)
		{
			// SPU executable (experimental)
			g_system = system_type::ps3;
			m_status = Ready;
			vm::ps3::init();
			spu_load_exec(spu_exec);
		}
		else if (arm_exec.open(elf_file) == elf_error::ok)
		{
			// ARMv7 executable
			g_system = system_type::psv;
			m_status = Ready;
			vm::psv::init();

			if (m_elf_path.empty())
			{
				m_elf_path = "host_root:" + m_path;
				LOG_NOTICE(LOADER, "Elf path: %s", m_elf_path);
			}

			arm_load_exec(arm_exec);
		}
		else
		{
			LOG_ERROR(LOADER, "Invalid or unsupported file format: %s", m_path);

			LOG_WARNING(LOADER, "** ppu_exec -> %s", ppu_exec.get_error());
			LOG_WARNING(LOADER, "** ppu_prx  -> %s", ppu_prx.get_error());
			LOG_WARNING(LOADER, "** spu_exec -> %s", spu_exec.get_error());
			LOG_WARNING(LOADER, "** arm_exec -> %s", arm_exec.get_error());
			return;
		}

		debug::autopause::reload();
		if (g_cfg_autostart) Run();
	}
	catch (const std::exception& e)
	{
		LOG_FATAL(LOADER, "%s thrown: %s", typeid(e).name(), e.what());
		Stop();
	}
}

void Emulator::Run()
{
	if (!IsReady())
	{
		Load();
		if(!IsReady()) return;
	}

	if (IsRunning()) Stop();

	if (IsPaused())
	{
		Resume();
		return;
	}

	rpcs3::on_run()();

	m_pause_start_time = 0;
	m_pause_amend_time = 0;
	m_status = Running;

	auto on_select = [](u32, cpu_thread& cpu)
	{
		cpu.run();
	};

	idm::select<ppu_thread>(on_select);
	idm::select<ARMv7Thread>(on_select);
	idm::select<RawSPUThread>(on_select);
	idm::select<SPUThread>(on_select);
}

bool Emulator::Pause()
{
	const u64 start = get_system_time();

	// Try to pause
	if (!m_status.compare_and_swap_test(Running, Paused))
	{
		return false;
	}

	rpcs3::on_pause()();

	// Update pause start time
	if (m_pause_start_time.exchange(start))
	{
		LOG_ERROR(GENERAL, "Emulator::Pause() error: concurrent access");
	}

	auto on_select = [](u32, cpu_thread& cpu)
	{
		cpu.state += cpu_flag::dbg_global_pause;
	};

	idm::select<ppu_thread>(on_select);
	idm::select<ARMv7Thread>(on_select);
	idm::select<RawSPUThread>(on_select);
	idm::select<SPUThread>(on_select);

	if (auto mfc = fxm::check<mfc_thread>())
	{
		on_select(0, *mfc);
	}

	return true;
}

void Emulator::Resume()
{
	// Get pause start time
	const u64 time = m_pause_start_time.exchange(0);

	// Try to increment summary pause time
	if (time)
	{
		m_pause_amend_time += get_system_time() - time;
	}

	// Try to resume
	if (!m_status.compare_and_swap_test(Paused, Running))
	{
		return;
	}

	if (!time)
	{
		LOG_ERROR(GENERAL, "Emulator::Resume() error: concurrent access");
	}

	auto on_select = [](u32, cpu_thread& cpu)
	{
		cpu.state -= cpu_flag::dbg_global_pause;
		cpu.notify();
	};

	idm::select<ppu_thread>(on_select);
	idm::select<ARMv7Thread>(on_select);
	idm::select<RawSPUThread>(on_select);
	idm::select<SPUThread>(on_select);

	if (auto mfc = fxm::check<mfc_thread>())
	{
		on_select(0, *mfc);
	}

	rpcs3::on_resume()();
}

void Emulator::Stop()
{
	if (m_status.exchange(Stopped) == Stopped)
	{
		return;
	}

	LOG_NOTICE(GENERAL, "Stopping emulator...");

	rpcs3::on_stop()();

	auto e_stop = std::make_exception_ptr(cpu_flag::dbg_global_stop);

	auto on_select = [&](u32, cpu_thread& cpu)
	{
		cpu.state += cpu_flag::dbg_global_stop;
		cpu.get()->set_exception(e_stop);
	};

	idm::select<ppu_thread>(on_select);
	idm::select<ARMv7Thread>(on_select);
	idm::select<RawSPUThread>(on_select);
	idm::select<SPUThread>(on_select);

	if (auto mfc = fxm::check<mfc_thread>())
	{
		on_select(0, *mfc);
	}

	LOG_NOTICE(GENERAL, "All threads signaled...");

	while (g_thread_count)
	{
		m_cb.process_events();

		std::this_thread::sleep_for(10ms);
	}

	LOG_NOTICE(GENERAL, "All threads stopped...");

	lv2_obj::cleanup();
	idm::clear();
	fxm::clear();

	LOG_NOTICE(GENERAL, "Objects cleared...");

	RSXIOMem.Clear();
	vm::close();
	ppu_finalize();

	if (g_cfg_autoexit)
	{
		GetCallbacks().exit();
	}
	else
	{
		Init();
	}
}

s32 error_code::error_report(const fmt_type_info* sup, u64 arg)
{
	logs::channel* channel = &logs::GENERAL;
	logs::level level = logs::level::error;
	const char* func = "Unknown function";

	if (auto thread = get_current_cpu_thread())
	{
		if (g_system == system_type::ps3 && thread->id_type() == 1)
		{
			auto& ppu = static_cast<ppu_thread&>(*thread);

			// Filter some annoying reports
			switch (arg)
			{
			case CELL_ESRCH:
			case CELL_EDEADLK:
			{
				if (ppu.m_name == "_cellsurMixerMain" || ppu.m_name == "_sys_MixerChStripMain")
				{
					if (std::memcmp(ppu.last_function, "sys_mutex_lock", 15) == 0 ||
						std::memcmp(ppu.last_function, "sys_lwmutex_lock", 17) == 0)
					{
						level = logs::level::trace;
					}
				}

				break;
			}
			}

			if (ppu.last_function)
			{
				func = ppu.last_function;
			}			
		}

		if (g_system == system_type::psv)
		{
			if (auto _func = static_cast<ARMv7Thread*>(thread)->last_function)
			{
				func = _func;
			}
		}
	}

	channel->format(level, "'%s' failed with 0x%08x%s%s", func, arg, sup ? " : " : "", std::make_pair(sup, arg));
	return static_cast<s32>(arg);
}

Emulator Emu;
