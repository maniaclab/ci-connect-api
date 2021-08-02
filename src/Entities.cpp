#include "Entities.h"

#include <boost/lexical_cast.hpp>

#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/transform_width.hpp>
#include <boost/archive/iterators/ostream_iterator.hpp>

#include <cppcodec/base32_rfc4648.hpp>
#include <random>

bool operator==(const User& u1, const User& u2){
	return(u1.valid==u2.valid && u1.unixName==u2.unixName);
}
bool operator!=(const User& u1, const User& u2){
	return(!(u1==u2));
}

std::ostream& operator<<(std::ostream& os, const User& u){
	if(!u)
		return os << "invalid user";
	os << u.unixName;
	if(!u.name.empty())
		os << " (" << u.name << ')';
	return os;
}

Group::Group(const GroupRequest& gr, std::string creationDate):
valid(gr.valid && !creationDate.empty()),
name(gr.name),displayName(gr.displayName),email(gr.email),phone(gr.phone),
purpose(gr.purpose),description(gr.description),creationDate(std::move(creationDate)),
unixID(gr.unixID),pending(true)
{}

bool operator==(const Group& g1, const Group& g2){
	return g1.name==g2.name;
}

std::ostream& operator<<(std::ostream& os, const Group& group){
	if(!group)
		return os << "invalid Group";
	return os << "group " << group.name;
}

bool operator==(const GroupRequest& g1, const GroupRequest& g2){
	return g1.name==g2.name;
}

std::ostream& operator<<(std::ostream& os, const GroupRequest& group){
	if(!group)
		return os << "invalid Group Creation Request";
	return os << "group creation request for " << group.name;
}

std::string GroupMembership::to_string(GroupMembership::Status status){
	switch(status){
		case NonMember: return "nonmember";
		case Pending: return "pending";
		case Active: return "active";
		case Admin: return "admin";
		case Disabled: return "disabled";
	}
}

GroupMembership::Status GroupMembership::from_string(const std::string& status){
	if(status=="nonmember")
		return NonMember;
	if(status=="pending")
		return Pending;
	if(status=="active")
		return Active;
	if(status=="admin")
		return Admin;
	if(status=="disabled")
		return Disabled;
	throw std::runtime_error("Invalid status string: "+status);
}

bool operator==(const GroupMembership& m1, const GroupMembership& m2){
	return(m1.valid==m2.valid && m1.userName==m2.userName && m1.groupName==m2.groupName);
}

std::ostream& operator<<(std::ostream& os, const GroupMembership& membership){
	if(!membership)
		return os << "invalid membership record";
	os << membership.userName << " memberhip in " << membership.groupName
	   << ": " << GroupMembership::to_string(membership.state);
	return os;
}

const std::string IDGenerator::userIDPrefix="user_";
const std::string IDGenerator::groupIDPrefix="group_";

std::string IDGenerator::generateRawID(){
	uint64_t value;
	{
		std::lock_guard<std::mutex> lock(mut);
		value=std::uniform_int_distribution<uint64_t>()(idSource);
	}
	std::ostringstream os;
	using namespace boost::archive::iterators;
	using base64_text=base64_from_binary<transform_width<const unsigned char*,6,8>>;
	std::copy(base64_text((char*)&value),base64_text((char*)&value+sizeof(value)),ostream_iterator<char>(os));
	std::string result=os.str();
	//convert to RFC 4648 URL- and filename-safe base64
	for(char& c : result){
		if(c=='+') c='-';
		if(c=='/') c='_';
	}
	return result;
}

std::string TOTPGenerator::generateRawTOTPSecret() {
	uint8_t buffer[16];
	{
		std::lock_guard<std::mutex> lock(mut);
		std::uniform_int_distribution<int> d(0, 255);
		for(int i = 0; i < 16; i++) {
			buffer[i] = d(idSource);
		}
	}

	using base32 = cppcodec::base32_rfc4648;
	std::string s = base32::encode(buffer);
	return s;
}
