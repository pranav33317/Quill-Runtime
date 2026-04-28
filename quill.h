#pragma once

#ifndef QUILL_H
#define QUILL_H

#include <cstdlib>
#include <functional>
#include <cstring>
#include <cstdio>
namespace quill {
	void init_runtime();
	void finalize_runtime();
	void start_finish();
	void end_finish();
	void async(std::function<void()> && lambda);
}

#endif
