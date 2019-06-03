#ifndef CONNECT_ENTITIES_H
#define CONNECT_ENTITIES_H

#include <memory>
#include <mutex>
#include <random>
#include <string>

///Represents a user account
struct User{
	User():valid(false),superuser(false){}
	explicit User(std::string name):
	valid(true),name(std::move(name)),superuser(false),serviceAccount(false){}
	
	///Indicates whether the account exists/is valid
	bool valid;
	std::string id;
	std::string name;
	std::string email;
	std::string phone;
	std::string institution;
	std::string token;
	std::string globusID;
	std::string sshKey;
	std::string unixName;
	bool superuser;
	///indicates that the account is used for some type of automation and should
	///be hidden form other users under typical circumstances
	bool serviceAccount;
	
	explicit operator bool() const{ return valid; }
};

bool operator==(const User& u1, const User& u2);
bool operator!=(const User& u1, const User& u2);
std::ostream& operator<<(std::ostream& os, const User& u);

struct Group{
	Group():valid(false){}
	explicit Group(std::string name):valid(true),name(std::move(name)){}
	
	///Indicates whether the Group exists/is valid
	bool valid;
	std::string name;
	std::string displayName;
	std::string email;
	std::string phone;
	std::string scienceField;
	std::string description;
	
	explicit operator bool() const{ return valid; }
};

///Compare groups by ID
bool operator==(const Group& g1, const Group& g2);
std::ostream& operator<<(std::ostream& os, const Group& group);

namespace std{
template<>
struct hash<Group>{
	using result_type=std::size_t;
	using argument_type=Group;
	result_type operator()(const argument_type& a) const{
		return(std::hash<std::string>{}(a.name));
	}
};
};

struct GroupMembership{
	GroupMembership():valid(false),state(NonMember){}
	
	///Indicates whether the record exists/is valid
	bool valid;
	std::string userID;
	std::string groupName;
	enum Status{
		NonMember,
		Pending,
		Active,
		Admin,
		Disabled
	} state;
	std::string stateSetBy;
	
	explicit operator bool() const{ return valid; }
	
	bool isMember() const{ return state==Active || state==Admin; }
	
	static std::string to_string(Status status);
	static Status from_string(const std::string& status);
};

///Compare group membership records
bool operator==(const GroupMembership& m1, const GroupMembership& m2);
std::ostream& operator<<(std::ostream& os, const GroupMembership& membership);

namespace std{
template<>
struct hash<GroupMembership>{
	using result_type=std::size_t;
	using argument_type=GroupMembership;
	result_type operator()(const argument_type& a) const{
		std::hash<std::string> sHash;
		return(sHash(a.userID)^sHash(a.groupName));
	}
};
}

static class IDGenerator{
public:
	///Creates a random ID for a new user
	std::string generateUserID(){
		return userIDPrefix+generateRawID();
	}
	///Creates a random ID for a new group
	std::string generateGroupID(){
		return groupIDPrefix+generateRawID();
	}
	///Creates a random access token for a user
	///At the moment there is no apparent reason that a user's access token
	///should have any particular structure or meaning. Definite requirements:
	/// - Each user's token should be unique
	/// - There should be no way for anyone to derive or guess a user's token
	///These requirements seem adequately satisfied by a block of 
	///cryptographically random data. Note that boost::uuids::random_generator 
	///uses /dev/urandom as a source of randomness, so this is not optimally
	///secure on Linux hosts. 
	std::string generateUserToken(){
		return generateRawID()+generateRawID();
	}
	
	const static std::string userIDPrefix;
	const static std::string groupIDPrefix;
	
private:
	std::mutex mut;
	std::random_device idSource;
	
	std::string generateRawID();
} idGenerator;

#endif //CONNECT_ENTITIES_H
