#include "translate.hpp"
#include <sys/mman.h>

std::string error = "";

bool inited = false;
size_t map_size;
char *map;

void init() {
	inited = true;
	FILE *tmp = fopen("instr.dat", "r");
	fseek(tmp, 0, SEEK_END);
	map_size = ftell(tmp);
	map = (char *)mmap(NULL, map_size, PROT_READ, MAP_PRIVATE, fileno(tmp), 0);
	fclose(tmp);
}

bool handle(std::string s, std::vector<std::string> args, const size_t linenum) {
	// handle prefixes (lock, repne, repe)
	if (s == "lock") {
		output_buffer.push_back(0xf0);
		s = args[0];
		args.erase(args.begin());
	} else if (s == "repne" || s == "repnz" || s == "bnd") {
		output_buffer.push_back(0xf2);
		s = args[0];
		args.erase(args.begin());
	} else if (s == "rep" || s == "repe" || s == "repz") {
		output_buffer.push_back(0xf3);
		s = args[0];
		args.erase(args.begin());
	}
	error = "";
	if (!inited)
		init();
	char *l = map;
	char *r = map + map_size - 1;
	// go to the start of the section
	while (r[-1] != '\n' || r[-2] != '\n')
		r--;
	// binary search for the section that matches
	while (l < r) {
		char *m = l + (r - l) / 2;
		while (m[-1] != '\n' || m[-2] != '\n')
			m--;
		if (strncmp(m, s.c_str(), s.size()) == 0 && m[s.size()] == ' ') {
			l = m;
			break;
		} else if (strncmp(m, s.c_str(), s.size()) < 0) {
			l = m + 6;
			while (l[-1] != '\n' || l[-2] != '\n')
				l++;
		} else {
			r = m - 6;
			while (r[-1] != '\n' || r[-2] != '\n')
				r--;
		}
	}
	// store all lines that match
	while ((strncmp(l, s.c_str(), s.size()) != 0 || l[s.size()] != ' ') && l < map + map_size)
		l++;
	if (l >= map + map_size - 1) {
		std::cerr << input_name << ":" << linenum << ": error: unrecognized instruction `" << s << "'" << std::endl;
		return false;
	}
	r = std::find(l + 1, map + map_size, '\n');
	std::vector<std::string> matches;
	while (*l != '\n' && r != map + map_size) {
		matches.push_back(std::string(l, r));
		l = r + 1;
		r = std::find(l, map + map_size, '\n');
	}
	std::vector<std::pair<enum op_type, short>> types;
	for (const std::string &arg : args) {
		enum op_type type = op_type(arg);
		if (type == REG) {
			types.push_back({REG, reg_size(arg)});
		} else if (type == MEM) {
			types.push_back({MEM, mem_size(arg)});
		} else if (type == IMM) {
			auto tmp = parse_imm(arg);
			if (tmp.second == -1) {
				std::cerr << input_name << ":" << linenum << ": error: " << error << std::endl;
				return false;
			} else if (tmp.second <= -2) {
				if (reloc_table.find(text_labels[tmp.first]) != reloc_table.end()) {
					int32_t off = reloc_table.at(text_labels.at(tmp.first)) - output_buffer.size() - 3;
					if ((int8_t)off == off) {
						types.push_back({IMM, 8});
					} else {
						types.push_back({IMM, 32});
					}
				} else {
					types.push_back({IMM, 32});
				}
			} else {
				types.push_back({IMM, tmp.second});
			}
		} else {
			return false;
		}
	}
	std::vector<std::pair<std::vector<std::string>, short>> valid;
	for (size_t i = 0; i < matches.size(); i++) {
		std::string &line = matches[i];
		std::vector<std::string> tokens;
		size_t l = 0;
		size_t r = line.find(' ');
		while (r != std::string::npos) {
			tokens.push_back(line.substr(l, r - l));
			l = r + 1;
			r = line.find(' ', l);
		}
		tokens.push_back(line.substr(l));
		if (tokens.size() != args.size() + 2)
			continue;
		bool matched = true;
		short size = 0;
		for (size_t j = 0; j < args.size(); j++) {
			std::string &token = tokens[j + 1];
			if (token[0] == 'R') {
				if (types[j].first != REG) {
					matched = false;
					break;
				}
				if (types[j].second != _sizes[token[1] - 'A']) {
					matched = false;
					break;
				}
				size += _sizes[token[1] - 'A'];
			} else if (token[0] == 'M') {
				if (types[j].first != MEM && types[j].first != REG) {
					matched = false;
					break;
				}
				if (token[1] == '*') {
					if (types[j].first != MEM) {
						matched = false;
						break;
					} else {
						continue;
					}
				}
				if (types[j].second != -1 && types[j].second != _sizes[token[1] - 'A']) {
					matched = false;
					break;
				}
				size += _sizes[token[1] - 'A'];
			} else if (token[0] == 'I') {
				if (types[j].first != IMM) {
					matched = false;
					break;
				}
				if (types[j].second > _sizes[token[1] - 'A']) {
					matched = false;
					break;
				}
			} else if (token[0] == 'L') {
				if (std::stoi(args[j]) != std::stoi(token.substr(1))) {
					matched = false;
					break;
				}
			} else if (isdigit(token[0]) || (token[0] >= 'a' && token[0] <= 'f')) {
				if (reg_num(args[j]) != std::stoi(token.substr(0, 1), nullptr, 16)) {
					matched = false;
					break;
				}
				if (types[j].second != _sizes[token[1] - 'A']) {
					matched = false;
					break;
				}
				size += _sizes[token[1] - 'A'];
			} else {
				std::cerr << "Error: misconfigured instruction set" << std::endl;
				return false;
			}
		}
		if (matched) {
			for (size_t j = 0; j < args.size(); j++) {
				if (types[j].second == -1) {
					types[j].second = _sizes[tokens[j + 1][1] - 'A'];
				}
			}
			std::vector<short> del;
			for (size_t j = 1; j < args.size(); j++) {
				if (isdigit(tokens[j][0]) || (tokens[j][0] >= 'a' && tokens[j][0] <= 'f'))
					del.push_back(j - 1);
				else if (tokens[j][0] == 'L')
					del.push_back(j - 1);
			}
			for (auto d : del) {
				args.erase(args.begin() + d);
				types.erase(types.begin() + d);
				tokens.erase(tokens.begin() + d + 1);
			}
			valid.push_back({tokens, size});
		}
	}
	if (valid.empty()) {
		std::cerr << input_name << ":" << linenum << ": error: invalid combination of opcode and operands" << std::endl;
		return false;
	}
	for (size_t i = 1; i < valid.size(); i++) {
		if (valid[i].second != valid[i - 1].second) {
			std::cerr << input_name << ":" << linenum << ": error: operation size not specified" << std::endl;
			return false;
		}
	}
	std::string best = "";
	size_t bestlen = -1;
	std::vector<short> bestreloc;
	for (auto &p : valid) {
		std::vector<short> reloc;
		std::string tmp = "";
		if (p.first.back()[0] == 'p') {
			tmp += std::stoi(p.first.back().substr(1, 2), nullptr, 16);
			p.first.back() = p.first.back().substr(3);
		}
		if (types.size() == 0) {
			for (size_t i = 0; i < p.first.back().size(); i += 2)
				tmp += std::stoi(p.first.back().substr(i, 2), nullptr, 16);
		} else if (types.size() == 1) {
			if (types[0].second == 16)
				tmp += 0x66;
			if (types[0].first == REG) {
				short a1 = reg_num(args[0]);
				a1 += (args[0][1] == 'h') * 4;
				size_t i = p.first.back()[0] == 'w';
				if (args[0][1] == 'h' && i) {
					std::cerr << input_name << ":" << linenum << ": error: invalid combination of opcode and operands" << std::endl;
					return false;
				}
				if (i || (types[0].second == 8 && args[0][1] != 'h' && a1 >= 4) || a1 >= 8)
					tmp += 0x40 | (i << 3) | (a1 & 8);
				short reg = 0;
				for (; i < p.first.back().size(); i += 2) {
					if (p.first.back()[i] == '/') {
						reg = std::stoi(p.first.back().substr(i + 1, 1), nullptr, 16);
						break;
					}
					tmp += std::stoi(p.first.back().substr(i, 2), nullptr, 16);
				}
				if (p.first[1][0] == 'R')
					tmp.back() += a1;
				else
					tmp += 0xc0 | a1;
				if (reg != -1)
					tmp.back() |= reg << 3;
			} else if (p.first[1][0] == 'M') {
				short s1 = _sizes[p.first[1][1] - 'A'];
				size_t w = p.first.back()[0] == 'w';
				mem_output *data = parse_mem(args[0], s1);
				if (data == nullptr) {
					if (error.empty())
						error = "invalid addressing mode";
					std::cerr << input_name << ":" << linenum << ": error: " << error << std::endl;
					return false;
				}
				if (data->prefix)
					tmp += data->prefix;
				if (data->rex)
					tmp += data->rex;
				else if (w)
					tmp += 0x48;
				short reg = 0;
				for (size_t i = w; i < p.first.back().size(); i += 2) {
					if (p.first.back()[i] == '/') {
						reg = std::stoi(p.first.back().substr(i + 1, 1), nullptr, 16);
						break;
					}
					tmp += std::stoi(p.first.back().substr(i, 2), nullptr, 16);
				}
				tmp += (uint8_t)data->rm | (reg << 3);
				if (data->sib != 0x7fff)
					tmp += data->sib;

				if (data->reloc == 0)
					reloc.push_back(0x8000 | tmp.size());
				else if (data->reloc == 1)
					reloc.push_back(0x4000 | tmp.size());
				else if (data->reloc == 2)
					reloc.push_back(0x1000 | tmp.size());

				if (data->offsize == 1)
					tmp += data->offset;
				else if (data->offsize == 2) {
					tmp += data->offset;
					tmp += data->offset >> 8;
					tmp += data->offset >> 16;
					tmp += data->offset >> 24;
				}
			} else if (p.first[1][0] == 'I') {
				auto a1 = parse_imm(args[0]);
				short s1 = _sizes[p.first[1][1] - 'A'];
				size_t i = p.first.back()[0] == 'w';
				if (i)
					tmp += 0x48;
				for (; i < p.first.back().size(); i += 2) {
					if (p.first.back()[i] == '/') {
						tmp += std::stoi(p.first.back().substr(i + 1, 1), nullptr, 16);
						break;
					}
					tmp += std::stoi(p.first.back().substr(i, 2), nullptr, 16);
				}
				if (a1.second == -1) {
					std::cerr << input_name << ":" << linenum << ": error: " << error << std::endl;
					return false;
				} else if (a1.second == -2) {
					reloc.push_back(0x8000 | tmp.size());
				} else if (a1.second == -3) {
					reloc.push_back(0x4000 | tmp.size());
				} else if (a1.second == -4) {
					if (s1 == 8 && reloc_table.find(text_labels[a1.first]) != reloc_table.end()) {
						int32_t off = reloc_table.at(text_labels.at(a1.first)) - output_buffer.size() - tmp.size() - 1;
						if ((int8_t)off == off) {
							a1.first = off;
						} else {
							reloc.push_back(0x2000 | tmp.size());
						}
					} else {
						reloc.push_back(0x2000 | tmp.size());
					}
				}
				for (int i = 0; i < s1; i += 8)
					tmp += (a1.first >> i) & 0xff;
			}
		} else if (types.size() >= 2) {
			if ((types[0].first != IMM && types[0].second == 16) || (types[1].first != IMM && types[1].second == 16))
				tmp += 0x66;
			// index, size
			std::pair<short, short> reg{-1, -1};
			std::pair<short, short> mem{-1, -1};
			std::pair<short, short> imm{-1, -1};
			for (size_t i = 1; i <= args.size(); i++) {
				if (p.first[i][0] == 'R')
					reg = {i, types[i - 1].second};
				else if (p.first[i][0] == 'M')
					mem = {i, types[i - 1].second};
				else if (p.first[i][0] == 'I')
					imm = {i, types[i - 1].second};
			}
			if (mem.first == -1) {
				short a1 = reg_num(args[reg.first - 1]);
				size_t i = p.first.back()[0] == 'w';
				if (i || (a1 & 8))
					tmp += 0x48 | (a1 >= 8);
				for (; i < p.first.back().size(); i += 2) {
					if (p.first.back()[i] == '/') {
						tmp += std::stoi(p.first.back().substr(i + 1, 1), nullptr, 16);
						break;
					}
					tmp += std::stoi(p.first.back().substr(i, 2), nullptr, 16);
				}
				tmp.back() += a1 & 7;
				auto a2 = parse_imm(args[imm.first - 1]);
				short s2 = _sizes[p.first[imm.first][1] - 'A'];
				if (a2.second == -1) {
					std::cerr << input_name << ":" << linenum << ": error: " << error << std::endl;
					return false;
				} else if (a2.second == -2) {
					reloc.push_back(0x8000 | tmp.size());
				} else if (a2.second == -3) {
					reloc.push_back(0x4000 | tmp.size());
				} else if (a2.second == -4) {
					if (s2 == 8 && reloc_table.find(text_labels[a2.first]) != reloc_table.end()) {
						int32_t off = reloc_table.at(text_labels.at(a2.first)) - output_buffer.size() - tmp.size() - 1;
						if ((int8_t)off == off) {
							a2.first = off;
						} else {
							reloc.push_back(0x2000 | tmp.size());
						}
					} else {
						reloc.push_back(0x2000 | tmp.size());
					}
				}
				for (int i = 0; i < s2; i += 8)
					tmp += (a2.first >> i) & 0xff;
			} else {
				short rex = 0;
				short rm = 0x7fff;
				short sib = 0x7fff;
				mem_output *data;
				if (mem.first != -1) {
					short s1 = mem.second;
					data = parse_mem(args[mem.first - 1], s1);
					if (data == nullptr) {
						if (error.empty())
							error = "invalid addressing mode";
						std::cerr << input_name << ":" << linenum << ": error: " << error << std::endl;
						return false;
					}
					if (data->prefix)
						tmp += data->prefix;
					rex = data->rex;
					rm = data->rm;
					sib = data->sib;
				}
				if (p.first.back()[0] == 'w')
					rex |= 0x48;
				if (reg.first != -1) {
					short a1 = reg_num(args[reg.first - 1]);
					if (a1 & 8)
						rex |= 0x40 | ((a1 & 8) >> 1);
					if (rm == 0x7fff)
						rm = 0xc0 | ((a1 & 7) << 3);
					else
						rm |= (a1 & 7) << 3;
				}
				if (rex != 0)
					tmp += rex;
				for (size_t i = p.first.back()[0] == 'w'; i < p.first.back().size(); i += 2) {
					if (p.first.back()[i] == '/') {
						rm |= std::stoi(p.first.back().substr(i + 1, 1), nullptr, 16) << 3;
						break;
					}
					tmp += std::stoi(p.first.back().substr(i, 2), nullptr, 16);
				}
				if (rm != 0x7fff)
					tmp += rm;
				if (sib != 0x7fff)
					tmp += sib;

				if (data->reloc == 0)
					reloc.push_back(0x8000 | tmp.size());
				else if (data->reloc == 1)
					reloc.push_back(0x4000 | tmp.size());
				else if (data->reloc == 2)
					reloc.push_back(0x1000 | tmp.size());

				if (data->offsize == 1)
					tmp += data->offset;
				else if (data->offsize == 2) {
					tmp += data->offset;
					tmp += data->offset >> 8;
					tmp += data->offset >> 16;
					tmp += data->offset >> 24;
				}
				if (imm.first != -1) {
					auto a1 = parse_imm(args[imm.first - 1]);
					short s1 = _sizes[p.first[imm.first][1] - 'A'];
					if (a1.second == -1) {
						std::cerr << input_name << ":" << linenum << ": error: " << error << std::endl;
						return false;
					} else if (a1.second == -2) {
						reloc.push_back(0x8000 | tmp.size());
					} else if (a1.second == -3) {
						reloc.push_back(0x4000 | tmp.size());
					} else if (a1.second == -4) {
						if (s1 == 8 && reloc_table.find(text_labels[a1.first]) != reloc_table.end()) {
							int32_t off = reloc_table.at(text_labels.at(a1.first)) - output_buffer.size() - tmp.size() - 1;
							if ((int8_t)off == off) {
								a1.first = off;
							} else {
								reloc.push_back(0x2000 | tmp.size());
							}
						} else {
							reloc.push_back(0x2000 | tmp.size());
						}
					}
					for (int i = 0; i < s1; i += 8)
						tmp += (a1.first >> i) & 0xff;
				}
			}
		}
		if (tmp.size() < bestlen) {
			best = tmp;
			bestlen = tmp.size();
			bestreloc = reloc;
		}
		if (bestlen == 1)
			break;
	}
	for (auto reloc : bestreloc) {
		if (reloc & 0x8000)
			data_relocations.push_back(output_buffer.size() + (reloc & 0xff));
		else if (reloc & 0x4000)
			bss_relocations.push_back(output_buffer.size() + (reloc & 0xff));
		else if (reloc & 0x2000)
			text_relocations.push_back(output_buffer.size() + (reloc & 0xff));
		else if (reloc & 0x1000) {
			std::string label;
			for (size_t i = 0; i < types.size(); i++) {
				if (types[i].first == MEM) {
					label = args[i];
					label = label.substr(label.find_last_of(' ') + 1);
					label = label.substr(0, label.size() - 1);
					break;
				}
			}
			rel_relocations.push_back({output_buffer.size() + (reloc & 0xff), label});
		}
	}
	for (size_t i = 0; i < best.size(); i++)
		output_buffer.push_back(best[i]);
	return true;
}
