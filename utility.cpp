#include "utility.hpp"

short reg_num(std::string s) {
	short best;
	std::string best_match = "";
	for (auto &i : _reg_num) {
		if (strstr(s.c_str(), i.first.c_str()) != nullptr)
			if (i.first.size() > best_match.size()) {
				best = i.second;
				best_match = i.first;
			}
	}
	if (best_match == "")
		return -1;
	return best;
}

short reg_size(std::string s) {
	for (auto &i : _reg_size) {
		if (std::regex_match(s, (std::regex)i.first))
			return i.second;
	}
	return -1;
}

short mem_size(std::string s) {
	if (s.starts_with("byte "))
		return 8;
	if (s.starts_with("word "))
		return 16;
	if (s.starts_with("dword "))
		return 32;
	if (s.starts_with("qword "))
		return 64;
	return -1;
}

enum op_type op_type(std::string s) {
	// if in register table return REG
	if (reg_size(s) != -1)
		return REG;
	// in format "SIZE [...]" or "[]" return MEM or OFFSET
	if (s.find(' ') != std::string::npos || s[0] == '[') {
		size_t l = s.find('[');
		size_t r = s.find(']');
		if (l == std::string::npos || r == std::string::npos)
			return INVALID;
		return MEM;
	}
	// otherwise return IMM (immediate)
	return IMM;
}

// this function will NOT handle invalid input properly
std::deque<uint8_t> parse_mem(std::string in, short &size, short &reloc) {
	if (reg_size(in) != -1) {
		std::deque<uint8_t> out;
		short s1 = reg_size(in);
		short a1 = reg_num(in);
		if (s1 == 16)
			out.push_back(0x66);
		if (a1 >= 8 || s1 == 64)
			out.push_back(0x40 | ((s1 == 64) << 3) | (a1 >= 8));
		out.push_back(0xc0 | (a1 & 7));
		return out;
	}
	// off can be: label + num, label, num
	// [reg + reg * scale + off] SIB
	// [reg + reg * scale] SIB
	// [reg + reg + off] SIB
	// [reg + reg] SIB
	// [reg * scale + off] SIB
	// [reg * scale] SIB
	// [reg + off] NO SIB
	// [reg] NO SIB
	// [off] NO SIB
	if (size == -1 || in[0] != '[') {
		short tmp;
		if (in.starts_with("byte "))
			tmp = 8;
		else if (in.starts_with("word "))
			tmp = 16;
		else if (in.starts_with("dword "))
			tmp = 32;
		else if (in.starts_with("qword "))
			tmp = 64;
		else if (in.starts_with("oword "))
			tmp = 128;
		else {
			error = "operation size not specified";
			return {};
		}
		size = tmp;
		in = in.substr(5 + (tmp >= 32));
	}
	in = in.substr(0, in.size() - 1);
	std::vector<std::string> tokens;
	std::vector<char> ops;
	size_t l = 0;
	size_t r = std::min(in.find('*'), in.find("+"));
	while (l++ != std::string::npos) {
		tokens.push_back(in.substr(l, r - l));
		if (l != 1)
			ops.push_back(in[l - 1]);
		l = r;
		r = std::min(in.find('*', l + 1), std::min(in.find('+', l + 1), in.find('-', l + 1)));
	}
	if (tokens.size() == 0)
		return {};

	for (size_t i = 0; i < ops.size(); i++) {
		if (ops[i] == '-') {
			tokens[i + 1] = std::to_string(-std::stoi(tokens[i + 1]));
			ops[i] = '+';
		}
	}

	// resolve labels and combine with imms if possible
	if (data_labels.find(tokens[tokens.size() - 1]) != data_labels.end()) {
		tokens[tokens.size() - 1] = std::to_string(data_labels.at(tokens[tokens.size() - 1]));
		reloc = 0;
	} else if (bss_labels.find(tokens[tokens.size() - 1]) != bss_labels.end()) {
		tokens[tokens.size() - 1] = std::to_string(bss_labels.at(tokens[tokens.size() - 1]));
		reloc = 1;
	}
	if (tokens.size() > 1 && data_labels.find(tokens[tokens.size() - 2]) != data_labels.end()) {
		uint32_t tmp = data_labels.at(tokens[tokens.size() - 2]);
		if (ops[ops.size() - 1] == '+')
			tokens[tokens.size() - 2] = std::to_string(tmp + std::stoi(tokens[tokens.size() - 1], 0, 0));
		else
			return {};
		ops.pop_back();
		tokens.pop_back();
		reloc = 0;
	} else if (tokens.size() > 1 && bss_labels.find(tokens[tokens.size() - 2]) != bss_labels.end()) {
		uint32_t tmp = bss_labels.at(tokens[tokens.size() - 2]);
		if (ops[ops.size() - 1] == '+')
			tokens[tokens.size() - 2] = std::to_string(tmp + std::stoi(tokens[tokens.size() - 1], 0, 0));
		else
			return {};
		ops.pop_back();
		tokens.pop_back();
		reloc = 1;
	}

	std::deque<uint8_t> out;

	// check to see if we need to use SIB
	if (tokens.size() == 1 || (reg_size(tokens[1]) == -1 && ops[0] == '+')) {
		// no sib
		if (tokens.size() == 1) {
			short a1 = reg_num(tokens[0]);
			if (a1 != -1) {
				// register
				// size override
				if (reg_size(tokens[0]) == 32)
					out.push_back(0x67);
				// rex prefix if necessary
				if (a1 >= 8) {
					out.push_back(0x41);
					// check for collisions with rip relative addressing
					if (a1 == 13) {
						out.push_back(0x45);
						out.push_back(0x00);
						return out;
					}
					// collisions with SIB addressing
					if ((a1 & 7) == 0b100) {
						out.push_back(0x04);
						out.push_back(0b00100000 | (a1 & 7));
						return out;
					}
				}
				// modrm
				out.push_back(((a1 == 5) << 6) | (a1 & 7));
				// cannot directly address bp, so we do it with an offset of 0
				if (a1 == 5)
					out.push_back(0x00);
				else if (a1 == 4)
					// we also cannot directly address sp, so we add a sib byte with no index
					out.push_back(0x24);
			} else {
				// offset
				// modrm
				out.push_back(0x04);
				out.push_back(0b00100101);
				int off = std::stol(tokens[0], 0, 0);
				if (reloc == 0)
					reloc = out.size();
				else if (reloc == 1)
					reloc = out.size() | 0x8000;
				out.push_back(off & 0xff);
				out.push_back((off >> 8) & 0xff);
				out.push_back((off >> 16) & 0xff);
				out.push_back((off >> 24) & 0xff);
			}
		} else {
			// must be reg + offset
			short a1 = reg_num(tokens[0]);
			if (a1 == -1) {
				error = "symbol `" + tokens[0] + "' undefined";
				return {};
			}
			int off = std::stol(tokens[1], 0, 0);
			// size override
			if (reg_size(tokens[0]) == 32)
				out.push_back(0x67);
			// rex prefix if necessary
			if (a1 >= 8)
				out.push_back(0x41);
			// modrm
			out.push_back(0x80 | (a1 & 7));
			if ((a1 & 7) == 4)
				// we cannot directly address sp, so we add a sib byte with no index
				out.push_back(0x24);
			// offset
			if (reloc == 0)
				reloc = out.size();
			else if (reloc == 1)
				reloc = out.size() | 0x8000;
			out.push_back(off & 0xff);
			out.push_back((off >> 8) & 0xff);
			out.push_back((off >> 16) & 0xff);
			out.push_back((off >> 24) & 0xff);
		}
	} else {
		// sib
		short base;
		short index;
		short scale;
		int offset;
		if (ops[0] != '*') {
			base = reg_num(tokens[0]);
			tokens.erase(tokens.begin());
			ops.erase(ops.begin());
		} else
			base = -1;
		if (ops[ops.size() - 1] != '*' && reg_size(tokens[tokens.size() - 1]) == -1) {
			offset = std::stoul(tokens[tokens.size() - 1], 0, 0);
			tokens.pop_back();
			ops.pop_back();
		} else
			offset = 0;
		if (tokens.size() == 0) {
			index = -1;
			scale = 0;
		} else if (tokens.size() == 1) {
			index = reg_num(tokens[0]);
			scale = 1;
		} else if (tokens.size() == 2) {
			index = reg_num(tokens[0]);
			scale = std::stoi(tokens[1]);
		} else
			return {};
		if (scale == 1)
			scale = 0;
		else if (scale == 2)
			scale = 1;
		else if (scale == 4)
			scale = 2;
		else if (scale == 8)
			scale = 3;
		else
			return {};
		// size override
		if (reg_size(tokens[0]) == 32)
			out.push_back(0x67);
		// rex prefix if necessary
		if (base >= 8 || index >= 8)
			out.push_back(0x40 | ((index >= 8) << 1) | (base >= 8));
		if (base >= 8)
			out[0] |= 1;
		if (index >= 8)
			out[0] |= 2;
		bool force = false;
		if ((base & 7) == 5)
			force = true;
		if (base == -1) {
			base = 5;
			out[out.size() - 1] &= ~0x80;
		}
		if (index == -1)
			index = 4;
		// modrm
		out.push_back(0x04 | ((offset || force) << 7));
		// sib
		out.push_back((scale << 6) | ((index & 7) << 3) | (base & 7));
		// offset
		if (reloc == 0)
			reloc = out.size();
		else if (reloc == 1)
			reloc = out.size() | 0x8000;
		if (offset || force) {
			out.push_back(offset & 0xff);
			out.push_back((offset >> 8) & 0xff);
			out.push_back((offset >> 16) & 0xff);
			out.push_back((offset >> 24) & 0xff);
		}
	}
	return out;
}

std::pair<unsigned long long, short> parse_imm(std::string s) {
	// if label, return label
	if (s[0] == '.')
		s = prev_label + s;
	if (data_labels.find(s) != data_labels.end()) {
		return {data_labels.at(s), -2};
	} else if (bss_labels.find(s) != bss_labels.end()) {
		return {bss_labels.at(s), -3};
	} else if (text_labels_map.find(s) != text_labels_map.end()) {
		return {text_labels_map.at(s), -4};
	}
	// detect base
	int base = 0;
	if (s.starts_with("0x"))
		base = 16;
	else if (s.starts_with("0b"))
		base = 2;
	else
		base = 10;
	if (base != 10)
		s = s.substr(2);
	// parse
	bool neg = s[0] == '-';
	try {
		if (neg) {
			int64_t val = std::stoll(s, nullptr, base);
			short size = 64;
			if ((int8_t)val == val)
				size = 8;
			else if ((int16_t)val == val)
				size = 16;
			else if ((int32_t)val == val)
				size = 32;
			return {val, size};
		} else {
			uint64_t val = std::stoull(s, nullptr, base);
			short size = 64;
			if ((val & 0xffffffffffffff00) == 0)
				size = 8;
			else if ((val & 0xffffffffffff0000) == 0)
				size = 16;
			else if ((val & 0xffffffff00000000) == 0)
				size = 32;
			return {val, size};
		}
	} catch (std::invalid_argument) {
		error = "symbol `" + s + "' undefined";
		return {0, -1};
	} catch (std::out_of_range) {
		error = "overflow in immediate value";
		return {0, -1};
	}
}
