/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "hunspell.hxx"

#include <iostream>
#include <string>

int main(int argc, char **argv) {
	if (argc != 3) {
		return 1;
	}
	auto dictionary = Hunspell(argv[1], argv[2]);
	auto line = std::string();
	while (std::getline(std::cin, line)) {
		const auto word = line.substr(0, line.find('\t'));
		if (dictionary.spell(word)) {
			std::cout << line << '\n';
		}
	}
	return std::cin.bad() || std::cout.bad() ? 1 : 0;
}
