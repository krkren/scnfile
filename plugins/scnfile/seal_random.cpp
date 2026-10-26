// seal_random.cpp - new random game keys, from the system's random source. Not part of the
// plugin (which never makes keys): tools/scnseal.cpp and kagex-release build it.
//
// SPDX-License-Identifier: 0BSD

#include "seal.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt")
#elif defined(__APPLE__)
#include <stdlib.h>
#else
#include <cerrno>
#include <sys/random.h>
#endif

namespace scnseal {

Key randomKey() {
	Key key;
#if defined(_WIN32)
	if (BCryptGenRandom(NULL, key.data(), ULONG(key.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
		throw Error("scnseal: the system's random source failed");
	}
#elif defined(__APPLE__)
	arc4random_buf(key.data(), key.size());
#else
	size_t done = 0;
	while (done < key.size()) {
		ssize_t n = getrandom(key.data() + done, key.size() - done, 0);
		if (n < 0) {
			if (errno == EINTR) continue;
			throw Error("scnseal: the system's random source failed");
		}
		done += size_t(n);
	}
#endif
	return key;
}

} // namespace scnseal
