#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "rapidjson/document.h"

#include "Entities.h"
#include "HTTPRequests.h"
#include "Logging.h"
#include "Process.h"
#include "Utilities.h"

///Define an ordering for group memberships
///In this context we never mix memberships involving different users, so we
///ignore how those should be ordered. 
bool operator<(const GroupMembership& g1, const GroupMembership& g2){
	return g1.groupName<g2.groupName;
}
///Quick and dirty way to compare memberships with strings. 
///Also viable only because considering multiple users is not necessary.
bool operator==(const GroupMembership& g1, const std::string& groupName){
	return g1.groupName==groupName;
}

///Given a base groupfigure out the prefix corresponding to its enclosing group
///which can be removed to form relative group names
std::string computeGroupPrefixToRemove(const std::string& sourceGroup){
	//if the source group is root.foo.bar we want to remove the name of its 
	//enclosing group so that it becomes just bar, and subgroup roo.foo.bar.baz
	//becomes bar.baz
	//find the last dot in the source group name
	auto pos=sourceGroup.rfind('.');
	if(pos!=std::string::npos && pos+1<sourceGroup.size())
		return sourceGroup.substr(0,pos+1);
	return "";
}

struct ExtendedUser : public User{
	///\param data describing the user, as unvalidated JSON
	///\param disabled whether the user is considered disabled in this context
	///\param groupFilter group memberships not beginning with this prefix will
	///                   be ignored, and groups beginning with it will have any 
	///                   prefix corresponding to its enclosing group stripped off
	ExtendedUser(const rapidjson::Document& userData, bool disabled, const std::string& groupFilter):
	disabled(disabled){
		if(!userData.IsObject())
			log_fatal("User data is not a JSON object");
		
		if(!userData.HasMember("metadata") || !userData["metadata"].IsObject())
			log_fatal("User data does not have a metadata property or it is not an object");
		
		if(!userData["metadata"].HasMember("name") || !userData["metadata"]["name"].IsString())
			log_fatal("User metadata does not have a name property or it is not a string");
		name = userData["metadata"]["name"].GetString();
		
		if(!userData["metadata"].HasMember("email") || !userData["metadata"]["email"].IsString())
			log_fatal("User metadata does not have an email property or it is not a string");
		email = userData["metadata"]["email"].GetString();
		
		if(!userData["metadata"].HasMember("phone") || !userData["metadata"]["phone"].IsString())
			log_fatal("User metadata does not have a phone property or it is not a string");
		email = userData["metadata"]["phone"].GetString();
		
		if(!userData["metadata"].HasMember("institution") || !userData["metadata"]["institution"].IsString())
			log_fatal("User metadata does not have an institution property or it is not a string");
		email = userData["metadata"]["institution"].GetString();
		
		if(!userData["metadata"].HasMember("unix_name") || !userData["metadata"]["unix_name"].IsString())
			log_fatal("User metadata does not have a unix_name property or it is not a string");
		unixName = userData["metadata"]["unix_name"].GetString();
		
		if(!userData["metadata"].HasMember("public_key") || !userData["metadata"]["public_key"].IsString())
			log_fatal("User metadata does not have a public_key property or it is not a string");
		sshKey = userData["metadata"]["public_key"].GetString();
		
		if(!userData["metadata"].HasMember("X.509_DN") || !userData["metadata"]["X.509_DN"].IsString())
			log_fatal("User metadata does not have an X.509_DN property or it is not a string");
		x509DN = userData["metadata"]["X.509_DN"].GetString();
		
		if(!userData["metadata"].HasMember("unix_id") || !userData["metadata"]["unix_id"].IsInt())
			log_fatal("User metadata does not have a unix_id property or it is not an integer");
		unixID = userData["metadata"]["unix_id"].GetInt();
		
		if(!userData["metadata"].HasMember("service_account") || !userData["metadata"]["service_account"].IsBool())
			log_fatal("User metadata does not have a service_account property or it is not a boolean");
		serviceAccount = userData["metadata"]["service_account"].GetBool();
		
		if(!userData["metadata"].HasMember("group_memberships") || !userData["metadata"]["group_memberships"].IsArray())
			log_fatal("User metadata does not have a group_memberships property or it is not a list");
		
		auto groupPrefixToRemove=computeGroupPrefixToRemove(groupFilter);
		for(const auto& membership : userData["metadata"]["group_memberships"].GetArray()){
			if(!membership.HasMember("name") || !membership["name"].IsString())
				log_fatal("User group membership does not have a name property or it is not a string");
			if(!membership.HasMember("state") || !membership["state"].IsString())
				log_fatal("User group membership does not have a state property or it is not a string");
			GroupMembership gm;
			gm.userName=unixName;
			gm.groupName=membership["name"].GetString();
			gm.state=GroupMembership::from_string(membership["state"].GetString());
			if(gm.groupName!=groupFilter && gm.groupName.find(groupFilter+".")!=0)
				continue; //ignore groups outside the allowed prefix
			gm.groupName=gm.groupName.substr(groupPrefixToRemove.size());
			memberships.insert(gm);
		}
		
		//TODO: we could also check an unpack the access_token, join_date, 
		//last_use_time, and superuser fields, but they are currently not used
		valid=true;
	}
	
	///All relevant groups to which the user belongs 
	std::set<GroupMembership> memberships;
	///Whether this user should be disabled on this host
	bool disabled;
	
	///\return the user's group memberships serialized as a comma separated string of group names
	std::string membershipsAsList() const{
		std::ostringstream ss;
		bool first=true;
		for(const GroupMembership& gm : memberships){
			if(first)
				first=false;
			else
				ss << ',';
			ss << gm.groupName;
		}
		return ss.str();
	}
	
	///\return a group to which the user belongs which should be suitable as the
	///        user's default
	std::string defaultGroup() const{
		if(memberships.empty()){ log_fatal("User " << unixName << " has no group memberships"); }
		if(!memberships.empty())
			//memberships are sorted by group name, and the first group name 
			//will be a prefix, i.e. enclosing group, of at least some of the 
			//following groups
			return memberships.begin()->groupName;
		return ""; //should be unreachable
	}
};

//Enable comparision between user objects and raw user names. 
//This is useful for computing set differences between lists of users and names
bool operator<(const std::string& uName, const ExtendedUser& user){
	return uName<user.unixName;
}
bool operator<(const ExtendedUser& user, const std::string& uName){
	return user.unixName<uName;
}

bool operator<(const std::string& gName, const Group& group){
	return gName<group.name;
}
bool operator<(const Group& group, const std::string& gName){
	return group.name<gName;
}

bool operator<(const std::string& gName, const GroupMembership& group){
	return gName<group.groupName;
}
bool operator<(const GroupMembership& group, const std::string& gName){
	return group.groupName<gName;
}

struct byNameComparator{
	bool operator()(const User& u1, const User& u2) const{
		return u1.unixName < u2.unixName;
	}
	bool operator()(const Group& g1, const Group& g2) const{
		return g1.name < g2.name;
	}
	bool operator()(const GroupMembership& g1, const GroupMembership& g2) const{
		return g1.groupName < g2.groupName;
	}
};

///Check whether the process with the given PID is still running (or otherwise 
///exists in the process table). 
bool processExists(pid_t p){
	//this, bizarrely, appears to be the idiomatic way to see if a PID 
	//corresponds to an extant process
	int result=kill(p,0/*send no actual signal*/);
	if(result==-1){
		result=errno;
		if(result==ESRCH)
			return false;
		throw std::runtime_error("Failed to use kill to query PID: error "
		                         +std::to_string(result)+" ("+strerror(result)+")");
	}
	return true;
}

///A lock file which indicates that the process is running, and should not 
///interupt or be interupted by another instance of itself. 
///Intended to conform to the Linux Filesystem Hierarchy Standard, v3.0 section 5.9
///https://refspecs.linuxfoundation.org/FHS_3.0/fhs/ch05s09.html
class LockFile{
public:
	///\param name the name the file should be given, which will be placed 
	///            within a standard directory
	LockFile(std::string name):fullPath(
#ifdef __linux__
	                                    "/var/lock/"
#else // /var/lock may not exist on non-linux systems
	                                    "/var/tmp/"
#endif
										+name){
		//check whether the file aleady exists
		std::ifstream existing(fullPath);
		if(existing){
			//if it exists try to read the owning process's PID from it
			std::string oldPIDs;
			existing >> oldPIDs;
			existing.close();
			log_info("PID from " << fullPath << ": " << oldPIDs);
			std::istringstream ss(oldPIDs);
			pid_t oldPID;
			ss >> oldPID;
			if(!ss)
				log_warn("Unable to parse '" <<  oldPIDs << "' as a process ID");
			else{
				//abort if the owner is still around
				if(processExists(oldPID))
					log_fatal("Lock file " << fullPath << " already exists; cowardly refusing to continue");
				else
					log_warn("Lock file " << fullPath << " apparently held by defunct process; proceeding");
			}
		}
		//take ownership of the file by writing our own PID to it
		std::ofstream file(fullPath);
		if(!file)
			log_fatal("Unable to open " << fullPath << " for writing");
		file << std::setw(10) << std::right << getpid() << '\n';
		if(!file)
			log_fatal("Failed to write to lock file " << fullPath << "; cowardly refusing to continue");
	}
	
	~LockFile(){
		//attempt to release the lock by deleting the file
		int result=unlink(fullPath.c_str());
		if(result!=0){
			result=errno;
			log_error("Failed to unlink " << fullPath << ": error " << result
			          << " (" << strerror(result) << ')');
		}
	}
private:
	///Path to the lock file on disk
	std::string fullPath;
};

///A wrapper for additional logic to be applied when managing users and groups.
///Plug-ins may optionally act on any of several triggers such as creation or
///deletion of users and groups. They cannot (or should not) alter the main
///effects of provisioning, but may add additional side effects. 
class Plugin{
public:
	///Invoked before modifications begin to be made
	virtual bool start(){ return true; }
	///Invoked when a group has just been created
	///\param group the details of the newly created group
	virtual bool addGroup(const Group& group){ return true; }
	///Invoked when a group has just been deleted
	///\param groupName the name of the group which was deleted
	virtual bool removeGroup(const std::string& groupName){ return true; }
	///Invoked when a user account has just been created
	///\param user the details of the newly created user
	///\param homeDir the path to the new user's home directory
	virtual bool addUser(const ExtendedUser& user, const std::string& homeDir){ return true; }
	///Invoked when a user account is updated with new data
	///\param user the new details of the user
	///\param homeDir the path to the user's home directory
	virtual bool updateUser(const ExtendedUser& user, const std::string& homeDir){ return true; }
	///Invoked when a user account has just been deleted
	///\param userName the name of the user which was deleted
	virtual bool removeUser(const std::string& userName){ return true; }
	///Invoked after modifications are otherwise completed
	virtual bool finish(){ return true; }
};

///A plug-in which is implemented as an external executable.
///This class will invoke the executable to determine which customization points
///it implements, and then invoke it for each of those customization points. 
///The executable interface is expected to use simple textual arguments, where 
///the first argument passed (after the executable name) will always be a 
///command, which may then be followed by additional arguments relevant to that 
///context. The exit status of the plug-in executable is checked after each 
///invocation, with zero being treated as success and all other values as 
///failure.
///
///Plug-in Interface:
///plug-in SUPPORTED_COMMANDS
///        This should write a whitespace separated list of other commands which 
///        the plug-in supports to standard output. 
///plug-in START
///        Invoked before modifications begin to be made
///plug-in ADD_GROUP group_name group_full_name group_email group_phone
///        Invoked when a group has just been created
///plug-in REMOVE_GROUP group_name
///        Invoked when a group has just been deleted
///plug-in ADD_USER user_name user_home_dir user_full_name user_email user_phone user_institution
///        Invoked when a user account has just been created
///plug-in UPDATE_USER user_name user_home_dir user_full_name user_email user_phone user_institution
///        Invoked when a user account is updated with new data
///plug-in REMOVE_USER user_name
///        Invoked when a user account has just been deleted
///plug-in FINISH
///        Invoked after modifications are otherwise completed
class ExternalPlugin : public Plugin{
public:
	///\param plugin the external executable to invoke
	explicit ExternalPlugin(std::string plugin):
	name(plugin),
	usesStart(false), 
	usesAddGroup(false), usesRemoveGroup(false), 
	usesAddUser(false), usesUpdateUser(false), usesRemoveUser(false), 
	usesFinish(false)
	{
		//check what commands the plug-in supports
		commandResult supported;
		try{
			supported=runCommand(name,{"SUPPORTED_COMMANDS"});
		}catch(std::runtime_error& ex){
			log_fatal("Failed to initialize plug-in " << name << ": " << ex.what());
		}
		if(supported.status!=0)
			throw std::runtime_error("Failed to query commands supported by the '"+name+"' plug-in");
		std::istringstream ss(supported.output);
		std::string command;
		while(ss >> command){
			//log_info("Plug-in " << name << " supports " << command);
			if(command=="START")
				usesStart=true;
			else if(command=="ADD_GROUP")
				usesAddGroup=true;
			else if(command=="REMOVE_GROUP")
				usesRemoveGroup=true;
			else if(command=="ADD_USER")
				usesAddUser=true;
			else if(command=="UPDATE_USER")
				usesUpdateUser=true;
			else if(command=="REMOVE_USER")
				usesRemoveUser=true;
			else if(command=="FINISH")
				usesFinish=true;
			else
				log_error(name << " plug-in claims to support the unknown command '" << command << "'");
		}
	}
	
	bool start() override{
		if(!usesStart)
			return true;
		auto result=runCommand(name,{"START"});
		if(result.status!=0)
			log_error("Plug-in " << name << ": START failed: " << result.error);
		return result.status==0; 
	}
	
	bool addGroup(const Group& group) override{
		if(!usesAddGroup)
			return true;
		auto result=runCommand(name,{"ADD_GROUP",group.name,group.displayName,group.email,group.phone});
		if(result.status!=0)
			log_error("Plug-in " << name << ": ADD_GROUP failed: " << result.error);
		return result.status==0; 
	}
	
	bool removeGroup(const std::string& groupName) override{
		if(!usesRemoveGroup)
			return true;
		auto result=runCommand(name,{"REMOVE_GROUP",groupName});
		if(result.status!=0)
			log_error("Plug-in " << name << ": REMOVE_GROUP failed: " << result.error);
		return result.status==0; 
	}
	
	bool addUser(const ExtendedUser& user, const std::string& homeDir) override{
		if(!usesAddUser)
			return true;
		auto result=runCommand(name,{"ADD_USER",user.unixName,homeDir,user.name,user.email,user.phone,user.institution,user.sshKey,user.x509DN});
		if(result.status!=0)
			log_error("Plug-in " << name << ": ADD_USER failed: " << result.error);
		return result.status==0; 
	}
	
	bool updateUser(const ExtendedUser& user, const std::string& homeDir) override{
		if(!usesUpdateUser)
			return true;
		auto result=runCommand(name,{"UPDATE_USER",user.unixName,homeDir,user.name,user.email,user.phone,user.institution,user.sshKey,user.x509DN});
		if(result.status!=0)
			log_error("Plug-in " << name << ": UPDATE_USER failed: " << result.error);
		return result.status==0; 
	}
	
	bool removeUser(const std::string& userName) override{
		if(!usesRemoveUser)
			return true;
		auto result=runCommand(name,{"REMOVE_USER",userName});
		if(result.status!=0)
			log_error("Plug-in " << name << ": REMOVE_USER failed: " << result.error);
		return result.status==0; 
	}
	
	bool finish() override{
		if(!usesFinish)
			return true;
		auto result=runCommand(name,{"FINISH"});
		if(result.status!=0)
			log_error("Plug-in " << name << ": FINISH failed: " << result.error);
		return result.status==0; 
	}
	
private:
	///The executable for this plug-in
	std::string name;
	bool usesStart, usesAddGroup, usesRemoveGroup, usesAddUser, usesUpdateUser, usesRemoveUser, usesFinish; 
};

///A plug-in which sets users' public SSH keys to allow interactive logins.
///This plug-in is probably always desirable, so it is built in. 
class SshPlugin : public Plugin{
public:
	///Set authorized keys when each user is created
	bool addUser(const ExtendedUser& user, const std::string& homeDir) override{ return setUserSSHKeys(user,homeDir); }
	///Update each users' authorized keys on every subsequent update
	bool updateUser(const ExtendedUser& user, const std::string& homeDir) override{ return setUserSSHKeys(user,homeDir); }
private:
	///Write a user's public keys to ~/.ssh/authorized_keys
	bool setUserSSHKeys(const ExtendedUser& user, const std::string& homeDir) const{
		//first make sure the home directory is where we expect it to be
		struct stat info;
		int err=stat(homeDir.c_str(),&info);
		if(err){
			err=errno;
			log_error("Unable to stat " << homeDir << ": " << strerror(err));
			return false;
		}
		//next check for the user's .ssh directory
		std::string sshDir=homeDir+"/.ssh";
		err=stat(sshDir.c_str(),&info);
		if(err){
			err=errno;
			if(err==ENOENT){ //directory doesn't exist; try to create it
				err=mkdir(sshDir.c_str(), S_IRWXU);
				if(err){
					err=errno;
					log_error("Unable to create " << sshDir << ": " << strerror(err));
					return false;
				}
				//make the directory be owned by the right user
				err=chown(sshDir.c_str(), user.unixID, -1);
				if(err){
					err=errno;
					log_error("Unable to set ownership of " << sshDir << ": " << strerror(err));
					return false;
				}
			}
			else{
				log_error("Unable to stat user ssh dir: " << strerror(err));
				return false;
			}
		}
		//write the updated keys
		std::string tempPath=sshDir+"/authorized_keys.new";
		{
			std::ofstream outfile(tempPath);
			outfile << user.sshKey << std::endl;
			if(!outfile){
				log_error("Failed to write SSH keys to " << tempPath);
				return false;
			}
		}
		//make the new key file be owned by the user
		err=chown(tempPath.c_str(), user.unixID, -1);
		if(err){
			err=errno;
			log_error("Unable to set ownership of " << tempPath << ": " << strerror(err));
			return false;
		}
		//make the new key file have correct permissions
		err=chmod(tempPath.c_str(), S_IRUSR|S_IWUSR);
		if(err){
			err=errno;
			log_error("Unable to set permissions on " << tempPath << ": " << strerror(err));
			return false;
		}
		//move the updated file into place
		err=rename(tempPath.c_str(),(sshDir+"/authorized_keys").c_str());
		if(err!=0){
			err=errno;
			log_error("Failed to replace " << (sshDir+"/authorized_keys") << ": error " << err
					  << " (" << strerror(err) << ')');
			return false;
		}
		
		return false;
	}
};

///An object which keeps track of what users and groups we are managing on this system
///It is very important that we not lose track of this data, so we should try to 
///get it written back out even if we have to abort part way through due to a problem.
class SystemState{
public:
	SystemState(std::string dataPrefix, bool dryRun, bool removeHomeDirs, 
				std::string homeDirRoot, const std::set<std::string>& pluginNames):
	dataPrefix(dataPrefix),dryRun(dryRun),removeHomeDirs(removeHomeDirs),
	homeDirRoot(homeDirRoot){
		if(!dataPrefix.empty() && dataPrefix.back()!='/')
			dataPrefix+='/';
		readExistingUsers();
		readExistingGroups();
		sanityCheckExistingRecords("/etc/passwd", existingUsers, "User");
		sanityCheckExistingRecords("/etc/group", existingGroups, "Group");
		
		//insert the SSH plug-in before any user-specified ones
		plugins.emplace_back(new SshPlugin);
		for(const auto& pluginName : pluginNames)
			plugins.emplace_back(new ExternalPlugin(pluginName));
		if(!dryRun){
			for(const auto& plugin : plugins)
				plugin->start();
		}
	}
	
	~SystemState(){
		//make sure updated lists get written
		bool okay=true;
		okay&=writeUpdatedExistingUsers();
		okay&=writeUpdatedExistingGroups();
		if(!okay)
			log_error("State not properly saved to disk");
		if(!dryRun){
			for(const auto& plugin : plugins)
				plugin->finish();
		}
	}
	
	void readExistingUsers(){ readExistingList("existing_users",existingUsers); }
	void readExistingGroups(){ readExistingList("existing_groups",existingGroups); }
	
	bool writeUpdatedExistingUsers() const{ return writeUpdatedList("existing_users",existingUsers); }
	bool writeUpdatedExistingGroups() const{ return writeUpdatedList("existing_groups",existingGroups); }
	
	const std::set<std::string>& getExistingUsers() const{ return existingUsers; }
	const std::set<std::string>& getExistingGroups() const{ return existingGroups; }
	
	///\return whether the target user was successfully added
	bool addUser(const ExtendedUser& user){
		std::string groups=user.membershipsAsList();
		std::cout << "Creating user " << user.unixName << " with uid " << user.unixID << " and groups " << groups << std::endl;
		if(!dryRun){
			auto result=runCommand("useradd",{
				"-c",user.name, //set full name
				"-u",std::to_string(user.unixID), //set uid
				"-m","-b",homeDirRoot, //create a home directory
				"-N","-g",user.defaultGroup(), //set the default group
				"-G",groups, //set additional groups
				user.unixName
			});
			if(result.status!=0){
				log_error("Failed to create user " << user.unixName << ": Error " << result.status << " " << result.error);
				return false;
			}
			existingUsers.insert(user.unixName);
			
			std::string homeDirPath=homeDirRoot;
			if(!homeDirPath.empty() && homeDirPath.back()!='/')
				homeDirPath+='/';
			homeDirPath+=user.unixName;
			for(const auto& plugin : plugins)
				plugin->addUser(user,homeDirPath);
		}
		return true;
	}
	///\return whether the target group was successfully created
	bool addGroup(const Group& group){
		std::cout << "Creating group " << group.name << " with gid " << group.unixID << std::endl;
		if(!dryRun){
			auto result=runCommand("groupadd",{group.name,"-g",std::to_string(group.unixID)});
			if(result.status!=0){
				log_error("Failed to create group " << group.name << ": Error " << result.status << " " << result.error);
				return false;
			}
			existingGroups.insert(group.name);
			for(const auto& plugin : plugins)
				plugin->addGroup(group);
		}
		return true;
	}
	
	///\param name the unix account name to remove
	///\return whether the target user was successfully removed
	bool removeUser(const std::string& name){
		std::cout << "Deleting user " << name << std::endl;
		if(!dryRun){
			std::vector<std::string> args={name};
			if(removeHomeDirs)
				args.push_back("-v");
			auto result=runCommand("userdel",args);
			if(result.status!=0){
				log_error("Failed to remove user " << name << ": Error " << result.status << " " << result.error);
				return false;
			}
			existingUsers.erase(name);
			for(const auto& plugin : plugins)
				plugin->removeUser(name);
		}
		return true;
	}
	///\param name the unix group name to remove
	///\return whether the target group was successfully removed
	bool removeGroup(const std::string& name){
		std::cout << "Deleting group " << name << std::endl;
		if(!dryRun){
			auto result=runCommand("groupdel",{name});
			if(result.status!=0){
				log_error("Failed to remove group " << name << ": Error " << result.status << " " << result.error);
				return false;
			}
			existingGroups.erase(name);
			for(const auto& plugin : plugins)
				plugin->removeGroup(name);
		}
		return true;
	}
	
	///Does nothing by itself; just invokes plugins
	void updateUser(const ExtendedUser& user) const{
		auto groups=user.membershipsAsList();
		std::cout << "Updating " << user.unixName << " group memberships to " << groups << std::endl;
		if(!dryRun){
			auto modResult=runCommand("usermod",{
				user.unixName,
				"-c",user.name,
				"-g",user.defaultGroup(),
				"-G",groups
			});
			if(modResult.status!=0)
				log_error("Failed to user " << user.unixName << ": " << modResult.error);
			
			std::string homeDirPath=homeDirRoot;
			if(!homeDirPath.empty() && homeDirPath.back()!='/')
				homeDirPath+='/';
			homeDirPath+=user.unixName;
			for(const auto& plugin : plugins)
				plugin->updateUser(user,homeDirPath);
		}
	}
	
	///Check that a list of existing objects is correct by comparing with what the OS has on record
	///\param sysFile the path to the OS file to read, usually /etc/passwd
	///\param existingRecords our own list to cross check
	///\param objName type of object being checked, to use in error messages
	bool sanityCheckExistingRecords(const std::string& sysFile, 
	                                std::set<std::string>& existingRecords, 
	                                std::string objName){
		std::ifstream sys(sysFile);
		if(!sys)
			log_fatal("Unable to read " << sysFile);
		std::string line;
		std::set<std::string> foundRecords;
		while(std::getline(sys,line)){
			//skip comments
			auto pos=line.find('#');
			if(pos!=std::string::npos)
				line=line.substr(pos);
			//extract fist colon delimited entry, if any
			pos=line.find(':');
			if(pos==std::string::npos || pos==0)
				continue;
			foundRecords.insert(line.substr(0,pos));
		}
		std::vector<std::string> missingRecords;
		std::set_difference(existingRecords.begin(),existingRecords.end(),
							foundRecords.begin(),foundRecords.end(),
							std::back_inserter(missingRecords));
		for(auto record : missingRecords){
			log_error(objName << ' ' << record << " is expected to exist, but does not");
			existingRecords.erase(record); //remove bogus record
		}
		return missingRecords.empty();
	}
	
private:
	///The directory prefix where the state files should be found/written
	std::string dataPrefix;
	///If set, no changes should be made, so all wwrite operations are no-ops
	bool dryRun;
	///Whether to delete home directories and their contents when removing user accounts
	bool removeHomeDirs;
	///Users which we have provisioned on this system
	std::set<std::string> existingUsers;
	///Groups which we have provisioned on this system
	std::set<std::string> existingGroups;
	///Path to the directory within which user home directories are to be created
	std::string homeDirRoot;
	std::vector<std::unique_ptr<Plugin>> plugins;
	
	void readExistingList(std::string fileName, std::set<std::string>& dataStore){
		std::string filePath=dataPrefix+fileName;
		auto perm=checkPermissions(filePath);
		if(perm==PermState::DOES_NOT_EXIST){ //File doesn't exist
		 	if(!dryRun){
				//This is fine, but before we do anything which might need to be 
				//recorded there, make sure we can write to it.
				std::ofstream touch(filePath);
				if(!touch)
					log_fatal("Unable to write to " << filePath);
			}
			return;
		}
		std::ifstream infile(filePath);
		if(!infile)
			log_fatal("Unable to read from " << filePath);
		std::string name;
		while(infile >> name)
			dataStore.insert(name);
	}
	
	///\return true if the data were successfully written
	bool writeUpdatedList(std::string fileName, const std::set<std::string>& dataStore) const{
		if(dryRun) //pretend we wrote
			return true;
		std::string filePath=dataPrefix+fileName;
		std::string tempPath=dataPrefix+"temporary";
		{
			std::ofstream outfile(tempPath);
			if(!outfile)
				log_fatal("Unable to write to " << tempPath);
			for(const auto& name : dataStore)
				outfile << name << '\n';
			if(!outfile) //make sure no write operation failed along the way
				log_fatal("Unable to write to " << tempPath);
		}
		int result=rename(tempPath.c_str(),filePath.c_str());
		if(result!=0){
			result=errno;
			log_error("Failed to replace " << filePath << ": error " << result
					  << " (" << strerror(result) << ')');
			return false;
		}
		return true;
	}
};

///Fetch all subgroups of the source group
///\return a list of groups, sorted by name
std::vector<Group> fetchGroups(std::string sourceGroup, std::string apiEndpoint, std::string apiToken){
	std::string prefixToRemove=computeGroupPrefixToRemove(sourceGroup);
	
	auto extractGroup=[&prefixToRemove](const rapidjson::Value& data)->Group{
		Group g;
		if(!data.IsObject())
			log_fatal("Group data is not a JSON object");
		
		if(!data.HasMember("name") || !data["name"].IsString())
			log_fatal("Group data does not have a name property or it is not a string");
		g.name = data["name"].GetString();
		if(g.name.find(prefixToRemove)==0)
			g.name=g.name.substr(prefixToRemove.size());
		
		if(!data.HasMember("display_name") || !data["display_name"].IsString())
			log_fatal("Group data does not have a display_name property or it is not a string");
		g.displayName = data["display_name"].GetString();
		
		if(!data.HasMember("email") || !data["email"].IsString())
			log_fatal("Group data does not have a email property or it is not a string");
		g.email = data["email"].GetString();
		
		if(!data.HasMember("phone") || !data["phone"].IsString())
			log_fatal("Group data does not have a phone property or it is not a string");
		g.phone = data["phone"].GetString();
		
		//don't currently care about purpose, description, or creation date
		
		if(!data.HasMember("unix_id") || !data["unix_id"].IsInt())
			log_fatal("Group data does not have a unix_id property or it is not an integer");
		g.unixID = data["unix_id"].GetInt();
		
		if(!data.HasMember("pending") || !data["pending"].IsBool())
			log_fatal("Group data does not have a pending property or it is not a boolean");
		g.pending = data["pending"].GetBool();
		
		g.valid=true;
		return g;
	};
	
	std::vector<Group> groups;
	
	//first we need to ftech the record for the group itself
	std::string url=apiEndpoint+"/v1alpha1/groups/"+sourceGroup+"?token="+apiToken;
	auto result=httpRequests::httpGet(url);
	if(result.status!=200)
		log_fatal("Failed to fetch group data: HTTP status " << result.status);
	
	rapidjson::Document data;
	try{
		data.Parse(result.body.c_str());
	}catch(std::runtime_error& err){
		log_fatal("Subgroup list result data cannot be parsed as JSON");
	}
	if(!data.IsObject())
		log_fatal("Group data is not a JSON object");
	if(!data.HasMember("metadata") || !data["metadata"].IsObject())
		log_fatal("Group data does not have a 'metadata' property, or this property is not an object");
	groups.emplace_back(extractGroup(data["metadata"]));
	
	//then we can fetch all (transitive) subgroups
	url=apiEndpoint+"/v1alpha1/groups/"+sourceGroup+"/subgroups?token="+apiToken;
	result=httpRequests::httpGet(url);
	if(result.status!=200)
		log_fatal("Failed to fetch subgroup list: HTTP status " << result.status);
	try{
		data.Parse(result.body.c_str());
	}catch(std::runtime_error& err){
		log_fatal("Subgroup list result data cannot be parsed as JSON");
	}
	if(!data.IsObject())
		log_fatal("Subgroup list result data is not a JSON object");
	if(!data.HasMember("groups") || !data["groups"].IsArray())
		log_fatal("Subgroup list result data does not have a 'groups' property, or this property is not an array");
	for(const auto& entry : data["groups"].GetArray())
		groups.emplace_back(extractGroup(entry));
	
	std::sort(groups.begin(),groups.end(),byNameComparator{});
	return groups;
}

///Fetch all members of the source group
///\return a list of users, sorted by unix name
std::vector<ExtendedUser> fetchUsers(std::string sourceGroup, std::string apiEndpoint, std::string apiToken, std::string groupSource){
	std::string url=apiEndpoint+"/v1alpha1/groups/"+sourceGroup+"/members?token="+apiToken;
	auto result=httpRequests::httpGet(url);
	if(result.status!=200)
		log_fatal("Failed to fetch user list: HTTP status " << result.status);
	rapidjson::Document data;
	try{
		data.Parse(result.body.c_str());
	}catch(std::runtime_error& err){
		log_fatal("User list result data cannot be parsed as JSON");
	}
	if(!data.IsObject())
		log_fatal("User list result data is not a JSON object");
	if(!data.HasMember("memberships"))
		log_fatal("User list result data does not have a 'memberships' property");
	if(!data["memberships"].IsArray())
		log_fatal("User list result data memberships property is not a list");
	//figure out which membbers are in good standing and shouuld be processed further
	std::map<std::string,bool> userNames; //map usernames to disabled status
	std::size_t nActive=0, nDisabled=0;
	for(const auto& item : data["memberships"].GetArray()){
		if(!item.IsObject())
			log_fatal("Entry in group membership list is not an object");
		if(!item.HasMember("user_name") || !item["user_name"].IsString())
			log_fatal("Entry in group membership list does not have a 'user_name' property, or this property is not a string");
		if(!item.HasMember("state") || !item["state"].IsString())
			log_fatal("Entry in group membership list does not have a 'state' property, or this property is not a string");
		std::string userName=item["user_name"].GetString();
		std::string userStatus=item["state"].GetString();
		if(userStatus!="pending"){ //ignore pending users
			userNames.emplace(userName,userStatus=="disabled");
			(userStatus=="disabled"?nDisabled:nActive)++;
		}
	}
	std::cout << "Found " << userNames.size() << " members of group " << sourceGroup 
	  << ": " << nActive << " active, " << nDisabled << " disabled" << std::endl;
		
	std::vector<ExtendedUser> users;
	//request user data in blocks to reduce load on the API server (and reduce latency)
	const std::size_t blockSize=1000;
	std::size_t fetched=0;
	auto userIt=userNames.begin();
	while(fetched!=userNames.size()){
		//build a request for up to the next blockSize users
		std::size_t toFetch=userNames.size()-fetched;
		if(toFetch>blockSize)
			toFetch=blockSize;
		std::ostringstream request;
		request << '{';
		std::string separator="";
		for(std::size_t i=0; i<toFetch; i++,userIt++){
			request << separator << "\"/v1alpha1/users/" << userIt->first 
			        << "?token=" << apiToken << "\":{\"method\":\"GET\"}";
			separator=",";
		}
		request << '}';
		url=apiEndpoint+"/v1alpha1/multiplex?token="+apiToken;
		result=httpRequests::httpPost(url,request.str());
		if(result.status!=200){
			log_fatal("Failed to fetch user data block: HTTP status " << result.status);
		}
		try{
			data.Parse(result.body.c_str());
		}catch(std::runtime_error& err){
			log_fatal("User list result data cannot be parsed as JSON");
		}
		if(!data.IsObject())
			log_fatal("Multiplexed user data result is not a JSON object");
		for(const auto& entry : data.GetObject()){
			if(!entry.value.IsObject())
				log_fatal("User data result item is not a JSON object");
			if(!entry.value.HasMember("status") || !entry.value["status"].IsInt()
			   || entry.value["status"].GetInt()!=200)
				log_fatal("User data result item does not have a status property,"
				          " or does not have a status of 200");
			if(!entry.value.HasMember("body") || !entry.value["body"].IsString())
				log_fatal("User data result item does not have a body property "
				          "or the body is not a string");
			rapidjson::Document userData;
			try{
				userData.Parse(entry.value["body"].GetString());
			}catch(std::runtime_error& err){
				log_fatal("User data result body cannot be parsed as JSON");
			}
			if(!userData.HasMember("metadata") || !userData["metadata"].IsObject())
				log_fatal("User data does not have a metadata property or it is not an object");
			if(!userData["metadata"].HasMember("unix_name") || !userData["metadata"]["unix_name"].IsString())
				log_fatal("User metadata does not have a unix_name property or it is not a string");
			std::string unixName = userData["metadata"]["unix_name"].GetString();
			auto userIt=userNames.find(unixName);
			if(userIt==userNames.end())
				log_fatal("Got unexpected user record");
			
			users.emplace_back(userData,userIt->second,groupSource);
		}
		
		fetched+=toFetch;
	}
	
	std::sort(users.begin(),users.end(),byNameComparator{});
	return users;
}

struct Configuration{
	struct ParamRef{
		enum Type{String,Bool} type;
		union{
			std::reference_wrapper<std::string> s;
			std::reference_wrapper<bool> b;
		};
		ParamRef(std::string& s):type(String),s(s){}
		ParamRef(bool& b):type(Bool),b(b){}
		ParamRef(const ParamRef& p):type(p.type){
			switch(type){
				case String: s=p.s; break;
				case Bool: b=p.b; break;
			}
		}
		
		ParamRef& operator=(const std::string& value){
			switch(type){
				case String:
					s.get()=value;
					break;
				case Bool:
				{
					if(value=="true" || value=="True" || value=="1")
						b.get()=true;
					else
						b.get()=false;
					break;
				}
			}
			return *this;
		}
	};
	
	std::string apiToken;
	std::string apiEndpoint;
	std::string userGroup;
	std::string groupGroup;
	std::string homeBase;
	bool wipe;
	bool cleanHome;
	bool dryRun;
	bool help;
	std::set<std::string> plugins;
	
	std::map<std::string,ParamRef> options;
	
	Configuration(int argc, char* argv[]):
	apiToken(""),
	apiEndpoint("https://api.ci-connect.net:18080"),
	userGroup(""),
	groupGroup(""),
	homeBase("/home"),
	wipe(false),
	cleanHome(false),
	dryRun(false),
	help(false),
	options{
		{"api-token",apiToken},
		{"api-endpoint",apiEndpoint},
		{"user-group",userGroup},
		{"group-group",groupGroup},
		{"home-base",homeBase},
		{"wipe",wipe},
		{"clean-home",cleanHome},
		{"dry-run",dryRun},
	}
	{
		//check for environment variables
		for(auto& option : options)
			fetchFromEnvironment("CICONNECT_"+option.first,option.second);
		
		std::string configPath;
		fetchFromEnvironment("CICONNECT_config",configPath);
		if(!configPath.empty())
			parseFile({configPath});
		
		//interpret command line arguments
		for(int i=1; i<argc; i++){
			std::string arg(argv[i]);
			if(arg=="-h" || arg=="-?" || arg=="--help"){
				help=true;
				break;
			}
			if(arg.size()<=2 || arg[0]!='-' || arg[1]!='-'){
				log_error("Unknown argument ignored: '" << arg << '\'');
				continue;
			}
			auto eqPos=arg.find('=');
			std::string optName=arg.substr(2,eqPos-2);
			if(options.count(optName)){
				if(eqPos!=std::string::npos)
					options.find(optName)->second=arg.substr(eqPos+1);
				else if(options.find(optName)->second.type==ParamRef::Bool){
					//treat boolean flags without an explicit value as true
					options.find(optName)->second.b.get()=true;
				}
				else{
					if(i==argc-1)
						log_fatal("Missing value after "+arg);
					i++;
					options.find(arg.substr(2))->second=argv[i];
				}
			}
			else if(optName=="config"){
				if(eqPos!=std::string::npos)
					parseFile({arg.substr(eqPos+1)});
				else{
					if(i==argc-1)
						log_fatal("Missing value after "+arg);
					i++;
					parseFile({argv[i]});
				}
			}
			else if(optName=="plugin"){
				if(eqPos!=std::string::npos)
					plugins.insert(arg.substr(eqPos+1));
				else{
					if(i==argc-1)
						log_fatal("Missing value after "+arg);
					i++;
					plugins.insert(argv[i]);
				}
			}
			else
				log_error("Unknown argument ignored: '" << arg << '\'');
		}
	}
	
	//attempt to read the last file in files, checking that it does not appear
	//previously
	void parseFile(const std::vector<std::string>& files){
		assert(!files.empty());
		if(std::find(files.begin(),files.end(),files.back())<(files.end()-1)){
			log_error("Configuration file loop: ");
			for(const auto file : files)
				log_error("  " << file);
			log_fatal("Configuration parsing terminated");
		}
		std::ifstream infile(files.back());
		if(!infile)
			log_fatal("Unable to open " << files.back() << " for reading");
		std::string line;
		unsigned int lineNumber=1;
		while(std::getline(infile,line)){
			auto eqPos=line.find('=');
			std::string optName=line.substr(0,eqPos);
			std::string value=line.substr(eqPos+1);
			if(options.count(optName))
				options.find(optName)->second=value;
			else if(optName=="config"){
				auto newFiles=files;
				newFiles.push_back(value);
				parseFile(newFiles);
			}
			else
				log_error(files.back() << ':' << lineNumber 
						  << ": Unknown option ignored: '" << line << '\'');
			lineNumber++;
		}
	}
	
};

const std::string helpText=R"(Usage: sync_users [OPTION]...

    --home-base path
        Use path as the base path for home directories
        The default is /home
    --api-endpoint URL
        Use URL as the endpoint at which to contact the CI-Connect API
        The default is https://api.ci-connect.net:18080
    --group-group group
        Use group as the group membership source group, the group from which to 
        collect subgroups to which users may belong. This can be different from
        the user source group (specified with -u), but should probably be an
        enclosing group of the user source group.
    -h, --help
        Show this help message
    --api-token token
        Use token when contacting the CI-Connect API
    --user-group group
        Use group as the user source group, the group from which users are 
        selected to be provisioned
    --wipe
        Remove all users and groups previously provisioned. This operation will
        permanently destroy any data in users' home directories which has not 
        been copied elsewhere. 
    --dry-run
        Report changes which would be made, without actually making any. 
    --clean-home
        When deleting users, delete their home directories as well. 

    Any option may equivalently be set by setting an environment variable with 
    the same name and a CICONNECT_ prefix; e.g. --api-token may be specified by
    setting the variable CICONNECT_api-token. 
)";

int main(int argc, char* argv[]){
	try{
		Configuration config(argc, argv);
		if(config.help){
			std::cout << helpText;
			return 0;
		}
		if(config.dryRun)
			std::cout << "Dry run; no changes will be made" << std::endl;
		if(config.cleanHome)
			std::cout << "Home directories of deleted users will be erased" << std::endl;
		if(config.wipe){
			std::cout << "Warning: Erasing all provisioned users and groups in 5 seconds" << std::endl;
			std::this_thread::sleep_for(std::chrono::seconds(5));
		}
		else{ //these checks only matter if we are not wiping
			if(config.apiToken.empty()){
				std::cerr << "api-token not set" << std::endl;
				return 1;
			}
			if(config.userGroup.empty()){
				std::cerr << "user-group not set" << std::endl;
				return 1;
			}
			if(config.groupGroup.empty()){
				std::cerr << "group-group not set" << std::endl;
				return 1;
			}
		}
		startReaper();
		
		LockFile lock("connect_sync");
		SystemState state("",config.dryRun,config.cleanHome,config.homeBase,config.plugins);
		
		if(config.wipe){
			//erase users
			auto existingUsers=state.getExistingUsers(); //take a copy before making changes 
			for(auto& user : existingUsers)
				state.removeUser(user);
			//erase groups
			auto existingGroups=state.getExistingGroups(); //take a copy before making changes 
			for(auto& group : existingGroups)
				state.removeGroup(group);
			return 0;
		}
		
		//download the latest state to synchronize
		auto expectedGroups = fetchGroups(config.groupGroup,config.apiEndpoint,config.apiToken);
		auto expectedUsers = fetchUsers(config.userGroup,config.apiEndpoint,config.apiToken,config.groupGroup);
		
		//Group memberships are a bit tricky, sisnce the system will not let us 
		//delete a group with members, or add a user to a group which does not 
		//exist.
		//This means that before we can delete groups we need to make sure we 
		//have deleted or removed all of its members, and before we can add a 
		//group membership to a user we must create the group. This means that 
		// we must split updating users' group memberships into two phases; a 
		//removal phase before we delete any old groups, and an addition phase 
		//after we create any new groups. 
		//Creating new users should also happen after creating groups, so that 
		//any necessary groups are already in place.
		
		//Delete all existing users which should not exist
		//All existing users which are not still in the current user list must 
		//be removed
		std::vector<std::string> usersToDelete;
		std::set_difference(state.getExistingUsers().begin(),state.getExistingUsers().end(),
							expectedUsers.begin(),expectedUsers.end(),
							std::back_inserter(usersToDelete));
		for(auto defunctUser : usersToDelete)
			state.removeUser(defunctUser);
		
		//Figure out which users already exist and merely need to have their 
		//details synchronized
		std::vector<ExtendedUser> usersToUpdate;
		std::set_intersection(expectedUsers.begin(),expectedUsers.end(), //outputs drawn from first range
							  state.getExistingUsers().begin(),state.getExistingUsers().end(),
							  std::back_inserter(usersToUpdate));
		
		//Update users to remove any old group memberships which are no longer correct
		for(const auto& user : usersToUpdate){
			//Get the users actual group memberships which are set right now
			//Prefer `id` to `groups` as its output format is more reliable, 
			//particularly when dealing with the dubious GNU coreutils
			auto groupsResult=runCommand("id",{"-Gn",user.unixName});
			if(groupsResult.status!=0){
				log_error("Failed to get current group memberships for user " << user.unixName);
				continue;
			}
			//parse space separated group names into a set
			std::set<std::string> existingMemberships;
			std::istringstream ss(groupsResult.output);
			std::copy(std::istream_iterator<std::string>(ss),std::istream_iterator<std::string>(),
					  std::inserter(existingMemberships,existingMemberships.begin()));
			//only need to update if the memberships have changed
			if(existingMemberships.size()!=user.memberships.size() ||
			   !std::equal(user.memberships.begin(),user.memberships.end(),existingMemberships.begin())){
				//determine the groups the user should still be in, and serialize 
				//them to a string as a comma separated list
				std::ostringstream ss;
				std::ostream_iterator<std::string> keepIt(ss,",");
				std::set_intersection(existingMemberships.begin(),existingMemberships.end(),
									  user.memberships.begin(),user.memberships.end(),
									  keepIt);
				std::string membershipsToKeep=ss.str();
				if(!membershipsToKeep.empty() && membershipsToKeep.back()==',')
					membershipsToKeep.resize(membershipsToKeep.size()-1); //clip trailing comma
				std::cout << "Reducing " << user.unixName << " group memberships to " << membershipsToKeep << std::endl; 
				if(!config.dryRun){
					auto modResult=runCommand("usermod",{"-G",membershipsToKeep});
					if(modResult.status!=0)
						log_error("Failed to update group memberships for user " << user.unixName << ": " << modResult.error);
				}
			}
		}
		
		//Delete all existing groups which should not exist
		//all existing groups which are not still in the current group list must be removed
		std::vector<std::string> groupsToDelete;
		std::set_difference(state.getExistingGroups().begin(),state.getExistingGroups().end(),
							expectedGroups.begin(),expectedGroups.end(),
							std::back_inserter(groupsToDelete));
		for(auto defunctGroup : groupsToDelete)
			state.removeGroup(defunctGroup);
		
		//Create all groups which should exist and don't
		std::vector<Group> groupsToCreate;
		std::set_difference(expectedGroups.begin(),expectedGroups.end(),
							state.getExistingGroups().begin(),state.getExistingGroups().end(),
							std::back_inserter(groupsToCreate));
		for(const auto& group : groupsToCreate)
			state.addGroup(group);
		
		//Create all users which should exist and don't
		std::vector<ExtendedUser> usersToCreate;
		std::set_difference(expectedUsers.begin(),expectedUsers.end(),
							state.getExistingUsers().begin(),state.getExistingUsers().end(),
							std::back_inserter(usersToCreate));
		for(const auto& user : usersToCreate){
			if(user.serviceAccount)
				continue; //ignore service accounts
			state.addUser(user);
		}
		
		//Update all remaining users
		for(const auto& user : usersToUpdate)
			state.updateUser(user);
		
	}catch(std::exception& ex){
		std::cerr << "sync_users: Error: " << ex.what() << std::endl;
		return 1;
	}
}
