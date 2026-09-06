/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/local_socket_security.h"

#include <QtNetwork/QLocalSocket>

#ifndef Q_OS_WIN
#include <sys/socket.h>
#include <unistd.h>
#endif // !Q_OS_WIN

namespace Core {

bool LocalSocketPeerAllowed(not_null<QLocalSocket*> socket) {
	if (socket->state() != QLocalSocket::ConnectedState) {
		return false;
	}
#ifdef Q_OS_WIN
	return true;
#else // Q_OS_WIN
	const auto descriptor = socket->socketDescriptor();
	if (descriptor < 0 || descriptor > std::numeric_limits<int>::max()) {
		return false;
	}
#ifdef Q_OS_LINUX
	auto credentials = ucred();
	auto size = socklen_t(sizeof(credentials));
	return getsockopt(int(descriptor), SOL_SOCKET, SO_PEERCRED,
		&credentials, &size) == 0
		&& size == sizeof(credentials)
		&& credentials.uid == geteuid();
#else // Q_OS_LINUX
	auto user = uid_t(0);
	auto group = gid_t(0);
	return getpeereid(int(descriptor), &user, &group) == 0
		&& user == geteuid();
#endif // !Q_OS_LINUX
#endif // !Q_OS_WIN
}

} // namespace Core
