#include "mytar.h"
#include <cstring>
#include <filesystem>

#define  lf_oldnormal '\0'       /* normal disk file, unix compatible */
#define  lf_normal    '0'        /* normal disk file */
#define  lf_link      '1'        /* link to previously dumped file */
#define  lf_symlink   '2'        /* symbolic link */
#define  lf_chr       '3'        /* character special file */
#define  lf_blk       '4'        /* block special file */
#define  lf_dir       '5'        /* directory */
#define  lf_fifo      '6'        /* fifo special file */
#define  lf_contig    '7'        /* contiguous file */
#define  lf_longname  'L'

#pragma pack(1)
typedef struct _tar_head {
        union {
                struct {
                        char name[100];
                        char mode[8];
                        char uid[8];
                        char gid[8];
                        char size[12];
                        char mtime[12];
                        char chksum[8];
                        char type;
                        char link_name[100];
                        char ustar[8];
                        char owner[32];
                        char group[32];
                        char major[32];
                        char minor[32];
                };
                char block[512];
        };
	int itype;
	long long id;
}TAR_HEAD;

enum HeadType {
	HEAD = 1,
	BODY,
	LONGNAME_HEAD
};

static unsigned int oct2uint(const char* src, int read_size) {
        unsigned int result = 0;
        int i = 0;
        while(i < read_size) {
                unsigned int byte_num = (unsigned int)src[i++] - '0';
                result = (result << 3) | byte_num;
        }
        return result;
}

static int recusive_mkdir(const char* dirname) {
        const size_t len = strlen(dirname);
        if (!len)
                return -1;

        char* path = (char*)calloc(len + 1, sizeof(char));
        strncpy(path, dirname, len);

        if (path[len - 1] == '/')
                path[len - 1] = 0;

        for(char *p = path; *p; p++) {
                if (*p == '/') {
                        *p = '\0';

#ifdef  __linux__
                        std::filesystem::create_directory(path);
#elif   WIN32
                        create_directory(path);
#endif
                        *p = '/';
                }
        }

	free(path);
	return 0;
}

ifStreamPtr open_tar_file(const std::string& tarfile) {
	auto m = ifStreamPtr(new std::ifstream());
	m->open(tarfile, std::ifstream::in | std::ifstream::binary);
	return m;
}

static void arrange_block(const std::string& tarfile, mytar::BlockPtr bl) {
	if(!bl->is_longname) {
		bl->offsize += 512;
	} else {
		auto file = open_tar_file(tarfile);
		bl->offsize += 512;
		file->seekg(bl->offsize);
		TAR_HEAD* tar = new TAR_HEAD;
		file->read(tar->block, 512);
		bl->filesize = oct2uint(tar->size, 11);
		bl->offsize += 512; // point to start
		file->close();
	}
}

namespace mytar {
class Hub{	
	static Hub* m_instance;
public:
	std::map<std::string, std::map<std::string, BlockPtr>> m_result;
	static Hub* instance() {
		if(!m_instance) {
			m_instance = new Hub;
		}
		return m_instance;
	}

	BlockPtr get_block(const std::string tarfile, const std::string name);
};

Hub* Hub::m_instance = NULL;

BlockPtr Hub::get_block(const std::string tarfile, const std::string name) {
	auto it = m_result.find(tarfile);
	if(it != m_result.end()) {
		auto files = it->second;
		auto it2 = files.find(name);
		if(it2 != files.end()) {
			return it2->second;
		}
		else
			return nullptr;
	}
	return nullptr;
}

WholeTar::WholeTar(const char* file) : m_name(file) {
	m_file = open_tar_file(file);
}

WholeTar::~WholeTar() {
	m_file->close();
}

void WholeTar::parsing(std::function<void(std::map<std::string, BlockPtr>)> func) {
	std::queue<std::shared_ptr<TAR_HEAD>> judge_queue;
	bool longname_ = false;
	long long off_size_ = 0;
	unsigned read_size_ = 512;
	while(*m_file) {
		std::shared_ptr<TAR_HEAD> tar = std::make_shared<TAR_HEAD>();
		m_file->read(tar->block, read_size_);

		tar->id = off_size_;	
		off_size_ += read_size_;
		if(judge_queue.size() >= 2)
			judge_queue.pop();

		judge_queue.push(tar);
		auto prev = judge_queue.front();
		auto file_size = oct2uint(prev->size, 11);
		auto block_size = strlen(tar->block) + 1;

		if(prev->type == lf_longname
				&& file_size == block_size && !strncmp(prev->ustar, "ustar", 5)) {
			tar->itype = HeadType::LONGNAME_HEAD;
			longname_ = true;
		}
		else if(!strncmp(tar->ustar, "ustar", 5)){
			tar->itype = HeadType::HEAD;
		}
		else {
			tar->itype = HeadType::BODY;
		}

		if( longname_ && tar->itype == HeadType::LONGNAME_HEAD) {
			auto block = std::make_shared<Block>(tar->id, true, file_size);
			arrange_block(m_name, block);
			Hub::instance()->m_result[m_name].insert({tar->block, block});
		}
		else if(tar->itype == HeadType::HEAD && tar->type != lf_longname && prev->itype != HeadType::LONGNAME_HEAD) {
			auto block = std::make_shared<Block>(tar->id, true, file_size);
			arrange_block(m_name, block);
			Hub::instance()->m_result[m_name].insert({tar->name, block});
		}
	}

	func(Hub::instance()->m_result[m_name]);

	m_file->close();
}

void WholeTar::show_all_file() {
	for(auto it : Hub::instance()->m_result[m_name]) {
		std::cout << "offsize:" << it.second->offsize << " " << it.first << std::endl;
	}
}


bool WholeTar::extract_file(const std::string name) {
	auto block = Hub::instance()->get_block(m_name, name);

	if(block == nullptr)
		return false;

	auto start_pos = block->offsize;
	auto filesize = block->filesize;

	/// create file directly
	recusive_mkdir(name.c_str());
	std::ofstream o(name.c_str(), std::ofstream::binary);

	m_file = open_tar_file(m_name);
	m_file->seekg(start_pos);

	if(!filesize)
		return false;
	while(filesize) {
		auto need_size = 512; 
		if(filesize < 512)
			need_size = filesize; 
			
		char* buffer = new char[need_size];
		m_file->read(buffer, need_size);
		o.write(buffer, need_size);
		delete [] buffer;
		filesize -= need_size;
	}

	o.close();
	m_file->close();
	return true;
}

BlockPtr WholeTar::get_file_block(const std::string& name) {
	auto block = Hub::instance()->get_block(m_name, name);
	return block;
}

}
