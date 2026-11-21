#pragma once

// 构建时间，格式 YYYY-MM-DD HH:MM。
//
// 取自身 PE 头里的链接时间戳（FileHeader.TimeDateStamp）：它由链接器写入，
// 增量构建也能反映这一次的真实构建时刻。打包器把时间戳清零时，退回编译期
// 的 __DATE__ —— 那个只在 BuildStamp.cpp 自己被重编时才变，精度差一档，
// 但不会输出一个空值。
const char* JTBuildDateUtf8();
const wchar_t* JTBuildDateWide();
