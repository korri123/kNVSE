#pragma once

void SafeWrite8(UInt32 addr, UInt32 data);
void SafeWrite16(UInt32 addr, UInt32 data);
void SafeWrite32(UInt32 addr, UInt32 data);
void SafeWriteBuf(UInt32 addr, const char* data, UInt32 len);

// 5 bytes
template <typename T>
void WriteRelJump(UInt32 jumpSrc, T jumpTgt)
{
	// jmp rel32
	SafeWrite8(jumpSrc, 0xE9);
	SafeWrite32(jumpSrc + 1, reinterpret_cast<UInt32>(jumpTgt) - jumpSrc - 1 - 4);
}

void WriteRelCall(UInt32 jumpSrc, UInt32 jumpTgt);

template <typename T>
void WriteRelCall(UInt32 jumpSrc, T jumpTgt)
{
	WriteRelCall(jumpSrc, UInt32(jumpTgt));
}

template <typename T>
void WriteVirtualCall(UInt32 jumpSrc, T jumpTgt)
{
	SafeWrite8(jumpSrc - 6, 0xB8); // mov eax
	SafeWrite32(jumpSrc - 5, (UInt32)jumpTgt);
	SafeWrite8(jumpSrc - 1, 0x90); // nop
}

template <typename T>
void SafeWrite32(UInt32 addr, T data)
{
	SafeWrite32(addr, (UInt32)data);
}

// 6 bytes
void WriteRelJnz(UInt32 jumpSrc, UInt32 jumpTgt);
void WriteRelJle(UInt32 jumpSrc, UInt32 jumpTgt);

void PatchMemoryNop(ULONG_PTR Address, SIZE_T Size);