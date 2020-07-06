#include <Utilities.h>

#include <cerrno>
#include <cstdlib>
#include <memory>
#include <stdexcept>

#include <unistd.h>
#include <sys/stat.h>

#ifdef CONNECT_SERVER
#warning using boost
#include <boost/date_time/posix_time/posix_time.hpp>

std::string timestamp(){
       auto now = boost::posix_time::microsec_clock::universal_time();
       return to_simple_string(now)+" UTC";
}
#else
//for the client timestamps are less imortant, and we really don't want a boost dependency
std::string timestamp(){ return ""; }
#endif

bool fetchFromEnvironment(const std::string& name, std::string& target){
	char* val=getenv(name.c_str());
	if(val){
		target=val;
		return true;
	}
	return false;
}

std::string getHomeDirectory(){
	std::string path;
	fetchFromEnvironment("HOME",path);
	if(path.empty())
		throw std::runtime_error("Unable to locate home directory");
	if(path.back()!='/')
		path+='/';
	return path;
}

PermState checkPermissions(const std::string& path){
	struct stat data;
	int err=stat(path.c_str(),&data);
	if(err!=0){
		err=errno;
		if(err==ENOENT)
			return PermState::DOES_NOT_EXIST;
		//TODO: more detail on what failed?
		throw std::runtime_error("Unable to stat "+path);
	}
	//check that the current user is actually the file's owner
	if(data.st_uid!=getuid())
		return PermState::INVALID;
	return((data.st_mode&((1<<9)-1))==0600 ? PermState::VALID : PermState::INVALID);
}
