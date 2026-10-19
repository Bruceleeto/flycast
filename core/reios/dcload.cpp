/*
	dcload "/pc/" file system for ELF boots.

	When an ELF is booted and a folder named "pc" sits next to it, Flycast poses as
	dcload: KallistiOS finds the dcload magic at 0x8c004004 and calls the syscall
	pointer at 0x8c004008, which leads to a reios trap handled here. Paths under /pc/
	are served from that folder, and console writes go to stdout.
*/
#include "dcload.h"
#include "reios.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_if.h"
#include "log/Log.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <string>

#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace
{
constexpr u32 MAGIC_ADDR = 0x8c004004, MAGIC_VALUE = 0xdeadbeef;
constexpr u32 SYSCALL_PTR = 0x8c004008;
constexpr u32 DIRENT_ADDR = 0x8c004100;		// struct dirent returned by READDIR (name at +11)
constexpr u32 DIRENT_NAME = 11, NAME_MAX_LEN = 255;

enum Command {
	READ = 0, WRITE, OPEN, CLOSE, CREAT, LINK, UNLINK, CHDIR, CHMOD, LSEEK, FSTAT, TIME, STAT,
	UTIME, ASSIGNWRKMEM, EXIT, OPENDIR, CLOSEDIR, READDIR, GETHOSTINFO, GDBPACKET, REWINDDIR,
};

std::string root;	// host folder that /pc/ maps to; empty when not installed

#ifndef _WIN32
std::map<u32, DIR *> dirs;
u32 nextDir = 1000;

// A NUL-terminated guest string, or empty when it does not fit.
std::string guestString(u32 addr)
{
	std::string s;
	for (u32 i = 0; i < 1024; i++)
	{
		char c = (char)ReadMem8(addr + i);
		if (c == 0)
			return s;
		s += c;
	}
	return {};
}

// Host path for a /pc/ path, or empty for one that would leave the folder.
std::string hostPath(u32 addr)
{
	std::string path = guestString(addr);
	if (path.empty() || path.find("..") != std::string::npos)
		return {};
	if (path[0] != '/')
		path = "/" + path;
	return root + path;
}

u8 *guestBuffer(u32 addr, u32 size)
{
	return size == 0 ? nullptr : GetMemPtr(addr, size);
}

// dcload's open flags are newlib's.
int hostFlags(u32 flags)
{
	int host = (flags & 3) == 2 ? O_RDWR : (flags & 3) == 1 ? O_WRONLY : O_RDONLY;
	if (flags & 0x0008)
		host |= O_APPEND;
	if (flags & 0x0200)
		host |= O_CREAT;
	if (flags & 0x0400)
		host |= O_TRUNC;
	return host;
}

// dcload_stat_t: 60 bytes, KallistiOS kernel/arch/dreamcast/include/dc/dcload.h
int writeStat(const struct stat& st, u32 addr)
{
	u8 *p = guestBuffer(addr, 60);
	if (p == nullptr)
		return -1;
	memset(p, 0, 60);
	auto put16 = [p](u32 off, u32 v) { u16 x = (u16)v; memcpy(p + off, &x, 2); };
	auto put32 = [p](u32 off, u32 v) { memcpy(p + off, &v, 4); };
	put16(0, st.st_dev);
	put16(2, st.st_ino);
	put32(4, st.st_mode);
	put16(8, st.st_nlink);
	put16(10, st.st_uid);
	put16(12, st.st_gid);
	put16(14, st.st_rdev);
	put32(16, (u32)st.st_size);
	put32(20, (u32)st.st_atime);
	put32(28, (u32)st.st_mtime);
	put32(36, (u32)st.st_ctime);
	put32(44, (u32)st.st_blksize);
	put32(48, (u32)st.st_blocks);
	return 0;
}

int consoleWrite(u32 fd, u32 data, u32 len)
{
	u8 *p = guestBuffer(data, len);
	if (p == nullptr)
		return len == 0 ? 0 : -1;
	fwrite(p, 1, len, fd == 2 ? stderr : stdout);
	fflush(fd == 2 ? stderr : stdout);
	return (int)len;
}

int syscall(u32 cmd, u32 a, u32 b, u32 c)
{
	switch (cmd)
	{
	case READ: {
		if (a <= 2)
			return 0;
		u8 *p = guestBuffer(b, c);
		return p == nullptr ? (c == 0 ? 0 : -1) : (int)::read((int)a, p, c);
	}
	case WRITE: {
		if (a <= 2)
			return consoleWrite(a, b, c);
		u8 *p = guestBuffer(b, c);
		return p == nullptr ? (c == 0 ? 0 : -1) : (int)::write((int)a, p, c);
	}
	case OPEN: {
		std::string path = hostPath(a);
		if (path.empty())
			return -1;
		int fd = ::open(path.c_str(), hostFlags(b), 0644);
		DEBUG_LOG(REIOS, "dcload: open %s -> %d", path.c_str(), fd);
		return fd;
	}
	case CREAT: {
		std::string path = hostPath(a);
		return path.empty() ? -1 : ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	}
	case CLOSE:
		return a <= 2 ? 0 : ::close((int)a);
	case LINK: {
		std::string from = hostPath(a), to = hostPath(b);
		return from.empty() || to.empty() ? -1 : ::link(from.c_str(), to.c_str());
	}
	case UNLINK: {
		std::string path = hostPath(a);
		return path.empty() ? -1 : ::unlink(path.c_str());
	}
	case LSEEK:
		return (int)::lseek((int)a, (off_t)(s32)b, (int)c);
	case FSTAT: {
		struct stat st;
		return ::fstat((int)a, &st) != 0 ? -1 : writeStat(st, b);
	}
	case STAT: {
		std::string path = hostPath(a);
		struct stat st;
		return path.empty() || ::stat(path.c_str(), &st) != 0 ? -1 : writeStat(st, b);
	}
	case TIME:
		return (int)std::time(nullptr);
	case ASSIGNWRKMEM:
		return -1;	// as dcload-ip: no compression buffer wanted
	case OPENDIR: {
		std::string path = hostPath(a);
		DIR *dir = path.empty() ? nullptr : ::opendir(path.c_str());
		if (dir == nullptr)
			return 0;
		dirs[nextDir] = dir;
		return (int)nextDir++;
	}
	case CLOSEDIR: {
		auto it = dirs.find(a);
		if (it == dirs.end())
			return -1;
		::closedir(it->second);
		dirs.erase(it);
		return 0;
	}
	case READDIR: {
		auto it = dirs.find(a);
		struct dirent *entry = it == dirs.end() ? nullptr : ::readdir(it->second);
		u8 *p = guestBuffer(DIRENT_ADDR, DIRENT_NAME + NAME_MAX_LEN + 1);
		if (entry == nullptr || p == nullptr)
			return 0;
		memset(p, 0, DIRENT_NAME + NAME_MAX_LEN + 1);
		strncpy((char *)p + DIRENT_NAME, entry->d_name, NAME_MAX_LEN);
		return (int)DIRENT_ADDR;
	}
	case REWINDDIR: {
		auto it = dirs.find(a);
		if (it == dirs.end())
			return -1;
		::rewinddir(it->second);
		return 0;
	}
	case GETHOSTINFO:
		return 0;
	case EXIT:
		NOTICE_LOG(REIOS, "dcload: program exited (%d)", (int)a);
		return 0;
	default:
		WARN_LOG(REIOS, "dcload: unsupported syscall %d", cmd);
		return -1;
	}
}

void closeAll()
{
	for (auto& [id, dir] : dirs)
		::closedir(dir);
	dirs.clear();
}
#endif
}	// namespace

void dcload_install(const std::string& elfPath)
{
	root.clear();
#ifndef _WIN32
	closeAll();
	size_t slash = elfPath.find_last_of('/');
	std::string folder = (slash == std::string::npos ? std::string(".") : elfPath.substr(0, slash)) + "/pc";
	struct stat st;
	if (::stat(folder.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
		return;
	root = folder;
	WriteMem32(MAGIC_ADDR, MAGIC_VALUE);
	WriteMem32(SYSCALL_PTR, DCLOAD_HOOK_ADDR);
	WriteMem16(DCLOAD_HOOK_ADDR, REIOS_OPCODE);
	NOTICE_LOG(REIOS, "dcload: /pc/ is %s", root.c_str());
#endif
}

void dcload_syscall()
{
	Sh4Context& ctx = p_sh4rcb->cntx;
#ifndef _WIN32
	if (!root.empty())
	{
		ctx.r[0] = (u32)syscall(ctx.r[4], ctx.r[5], ctx.r[6], ctx.r[7]);
		return;
	}
#endif
	ctx.r[0] = (u32)-1;
}
