#pragma once
#include <functional>


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
	SafeWrite32(jumpSrc + 1, UInt32(jumpTgt) - jumpSrc - 1 - 4);
}

// Specialization for member function pointers
template <typename C, typename Ret, typename... Args>
void WriteRelJump(UInt32 source, Ret(C::* const target)(Args...) const) {
	union
	{
		Ret(C::* tgt)(Args...) const;
		UInt32 funcPtr;
	} conversion;
	conversion.tgt = target;

	WriteRelJump(source, conversion.funcPtr);
}

// Specialization for member function pointers
template <typename C, typename Ret, typename... Args>
void WriteRelJump(UInt32 source, Ret(C::* const target)(Args...)) {
	union
	{
		Ret(C::* tgt)(Args...);
		UInt32 funcPtr;
	} conversion;
	conversion.tgt = target;

	WriteRelJump(source, conversion.funcPtr);
}

void WriteRelCall(UInt32 jumpSrc, UInt32 jumpTgt);

template <typename T>
void WriteRelCall(UInt32 jumpSrc, T jumpTgt, UInt32* originalFunction = nullptr)
{
	if (originalFunction && *reinterpret_cast<UInt8*>(jumpSrc) == 0xE8)
		*originalFunction = *reinterpret_cast<UInt32*>(jumpSrc + 1) + jumpSrc + 5;
	WriteRelCall(jumpSrc, UInt32(jumpTgt));
}

template <typename C, typename Ret, typename... Args>
void WriteRelCall(UInt32 jumpSrc, Ret (C::*jumpTgt)(Args...)) {
	union
	{
		Ret (C::*tgt)(Args...);
		UInt32 funcPtr;
	} conversion;
	conversion.tgt = jumpTgt;

	WriteRelCall(jumpSrc, conversion.funcPtr);
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

UInt32 GetRelJumpAddr(UInt32 jumpSrc);

template <typename C, typename Ret, typename... Args>
void ReplaceVTableEntry(void** apVTable, uint32_t auiPosition, Ret(C::* const target)(Args...) const) {
	union {
		Ret(C::* tgt)(Args...) const;
		SIZE_T funcPtr;
	} conversion;
	conversion.tgt = target;

	apVTable[auiPosition] = (void*)conversion.funcPtr;
}

template <typename C, typename Ret, typename... Args>
void ReplaceVTableEntry(void** apVTable, uint32_t auiPosition, Ret(C::* const target)(Args...)) {
	union {
		Ret(C::* tgt)(Args...);
		SIZE_T funcPtr;
	} conversion;
	conversion.tgt = target;

	apVTable[auiPosition] = (void*)conversion.funcPtr;
}
