#ifndef CONNECT_PERSISTENT_STORE_H
#define CONNECT_PERSISTENT_STORE_H

#include <atomic>
#include <memory>
#include <set>
#include <string>

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/dynamodb/DynamoDBClient.h>

#include <libcuckoo/cuckoohash_map.hh>

#include <concurrent_multimap.h>
#include <Entities.h>
//#include <FileHandle.h>

//In libstdc++ versions < 5 std::atomic seems to be broken for non-integral types
//In that case, we must use our own, minimal replacement
#ifdef __GNUC__
	#ifndef __clang__ //clang also sets __GNUC__, unfortunately
		#if __GNUC__ < 5
			#include <atomic_shim.h>
			#define connect_atomic simple_atomic
		#endif
	#endif
#endif
//In all other circustances we want to use the standard library
#ifndef connect_atomic
	#define connect_atomic std::atomic
#endif

///A wrapper type for tracking cached records which must be considered 
///expired after some time
template <typename RecordType>
struct CacheRecord{
	using steady_clock=std::chrono::steady_clock;
	
	///default construct a record which is considered expired/invalid
	CacheRecord():expirationTime(steady_clock::time_point::min()){}
	
	///construct a record which is considered expired but contains data
	///\param record the cached data
	CacheRecord(const RecordType& record):
	record(record),expirationTime(steady_clock::time_point::min()){}
	
	///\param record the cached data
	///\param exprTime the time after which the record expires
	CacheRecord(const RecordType& record, steady_clock::time_point exprTime):
	record(record),expirationTime(exprTime){}
	
	///\param validity duration until the record expires
	template <typename DurationType>
	CacheRecord(const RecordType& record, DurationType validity):
	record(record),expirationTime(steady_clock::now()+validity){}
	
	///\param exprTime the time after which the record expires
	CacheRecord(RecordType&& record, steady_clock::time_point exprTime):
	record(std::move(record)),expirationTime(exprTime){}
	
	///\param validity duration until the record expires
	template <typename DurationType>
	CacheRecord(RecordType&& record, DurationType validity):
	record(std::move(record)),expirationTime(steady_clock::now()+validity){}
	
	///\return whether the record's expiration time has passed and it should 
	///        be discarded
	bool expired() const{ return (steady_clock::now() > expirationTime); }
	///\return whether the record has not yet expired, so it is still valid
	///        for use
	operator bool() const{ return (steady_clock::now() <= expirationTime); }
	///Implicit conversion to RecordType
	///\return the data stored in the record
	///This function is not available when it would be ambiguous because the 
	///stored data type is also bool.
	template<typename ConvType = RecordType>
	operator typename std::enable_if<!std::is_same<ConvType,bool>::value,ConvType>::type() const{ return record; }
	
	//The cached data
	RecordType record;
	///The time at which the cached data should be discarded
	steady_clock::time_point expirationTime;
};

///Two cache records are equivalent if their contained data is equal, regardless
///of expiration times
template <typename T>
bool operator==(const CacheRecord<T>& r1, const CacheRecord<T>& r2){
	return (const T&)r1==(const T&)r2;
}

namespace std{
///The hash of a cache record is simply the hash of its stored data; the 
///expiration time is irrelevant.
template<typename T>
struct hash<CacheRecord<T>> : public std::hash<T>{};
	
///Define the hash of a set as the xor of the hashes of the items it contains. 
template<typename T>
struct hash<std::set<T>>{
	using result_type=std::size_t;
	using argument_type=std::set<T>;
	result_type operator()(const argument_type& t) const{
		result_type result=0;
		for(const auto& item : t)
			result^=std::hash<T>{}(item);
		return result;
	}
};
}

class EmailClient{
public:
	struct Email{
		std::string fromAddress;
		std::vector<std::string> toAddresses;
		std::vector<std::string> ccAddresses;
		std::vector<std::string> bccAddresses;
		std::string replyTo;
		std::string subject;
		std::string body;
	};

	EmailClient(const std::string& mailgunEndpoint, 
	            const std::string& mailgunKey, const std::string& emailDomain);
	bool canSendEmail() const{ return valid; }
	bool sendEmail(const Email& email);
private:
	std::string mailgunEndpoint;
	std::string mailgunKey;
	std::string emailDomain;
	bool valid;
};

class PersistentStore{
public:
	///\param credentials the AWS credentials used for authenitcation with the 
	///                   database
	///\param clientConfig specification of the database endpoint to contact
	///\param bootstrapUserFile the path from which the initial portal user
	///                         (superuser) credentials should be loaded
	///\param encryptionKeyFile the path to the file from which the encryption 
	///                         key used to protect secrets should be loaded
	///\param appLoggingServerName server to which application instances should 
	///                            send monitoring data
	///\param appLoggingServerPort port to which application instances should 
	///                            send monitoring data
	PersistentStore(const Aws::Auth::AWSCredentials& credentials, 
	                const Aws::Client::ClientConfiguration& clientConfig,
	                std::string bootstrapUserFile, EmailClient emailClient);
	
	///Store a record for a new user
	///\param user the user to create. If the user does not have a unix ID number, 
	///            one will be assigned
	///\return Whether the user record was successfully added to the database
	bool addUser(User& user);
	
	///Find information about the user with a given ID
	///\param id the users ID
	///\return the corresponding user or an invalid user object if the id is not known
	User getUser(const std::string& id);
	
	///Find the user who owns the given access token. Currently does not bother 
	///to retreive the user's name, email address, or globus ID. 
	///\param token access token
	///\return the token owner or an invalid user object if the token is not known
	User findUserByToken(const std::string& token);
	
	///Find the user corresponding to the given Globus ID. Currently does not bother 
	///to retreive the user's name, email address, or admin status. 
	///\param globusID Globus ID to look up
	///\return the corresponding user or an invalid user object if the ID is not known
	User findUserByGlobusID(const std::string& globusID);
	
	///Change a user record
	///\param user the updated user record, with an ID matching the previous ID
	///\param user the previous user record
	///\return Whether the user record was successfully altered in the database
	bool updateUser(const User& user, const User& oldUser);
	
	///Delete a user record
	///\param id the ID of the user to delete
	///\return Whether the user record was successfully removed from the database
	bool removeUser(const std::string& id);
	
	///Compile a list of all current user records
	///\return all users, but with only IDs, names, and email addresses
	std::vector<User> listUsers();

	///Compile a list of all current user records for the given group
	///\return all users from the given group, but with only IDs, names, and email addresses
	//std::vector<GroupMembership> listUsersByGroup(const std::string& group);
	
	///Set a user's status within a group. 
	///Should not be used for non-member status, instead use removeUserFromGroup
	///\param uID the ID of the user to add
	///\param groupID the ID of the group to which to add the user
	///\return wther the addition operation succeeded
	bool setUserStatusInGroup(const GroupMembership& membership);
	
	///Remove a user from a group
	///\param uID the ID of the user to remove
	///\param groupID the ID of the group from which to remove the user
	///\return wther the removal operation succeeded
	bool removeUserFromGroup(const std::string& uID, std::string groupID);
	
	///List all groups of which a user is a member
	///\param uID the ID of the user to look up
	///\param useNames if true perform the necessary extra lookups to transform
	///                the group IDs into the more human-friendly names
	///\return the IDs or names of all groups to which the user belongs
	std::vector<GroupMembership> getUserGroupMemberships(const std::string& uID);
	
	///Check whether a user is a member of a group
	///\param uID the ID of the user to look up
	///\param groupID the ID of the group to look up
	///\return whether the user is a member of the group
	GroupMembership userStatusInGroup(const std::string& uID, std::string groupName);
	
	///\return whether the attribute was successfully recorded
	bool setUserSecondaryAttribute(const std::string& uID, const std::string& attributeName, const std::string& attributeValue);
	
	std::string getUserSecondaryAttribute(const std::string& uID, const std::string& attributeName);
	
	///\return whether the attribute was successfully deleted
	bool removeUserSecondaryAttribute(const std::string& uID, const std::string& attributeName);
	
	///\throws std::runtime_error o database query failure
	bool unixNameInUse(const std::string& name);
	
	//----
	
	///Create a record for a new group
	///\param group the group to create. If the group does not have a unix ID number, 
	///             one will be assigned
	///\pre the new group must have a unique ID and name
	///\return whether the addition operation was successful
	bool addGroup(Group& group);
	
	///Create a record for a group creation request
	///\param group the new group
	///\pre the new group must have a unique ID and name
	///\return whether the addition operation was successful
	bool addGroupRequest(const GroupRequest& gr);
	
	///Delete a group record
	///\param groupID the ID of the group to delete
	///\return Whether the user record was successfully removed from the database
	bool removeGroup(const std::string& groupName);
	
	///Change a group record
	///\param group the updated group record, which must have matching ID 
	///          with the old record
	///\return Whether the group record was successfully altered in the database
	bool updateGroup(const Group& group);
	
	bool updateGroupRequest(const GroupRequest& request);
	
	///Find all users who belong to a group
	///\groupID the ID of the group whose members are to be found
	///\return the IDs of all members of the group
	std::vector<GroupMembership> getMembersOfGroup(const std::string groupName);
	
	///Find all current groups
	///\return all recorded groups
	std::vector<Group> listGroups();
	
	std::vector<GroupRequest> listGroupRequests();
	
	std::vector<GroupRequest> listGroupRequestsByRequester(const std::string& requester);
	
	///Find the group, if any, with the given UUID or name
	///\param idOrName the UUID or name of the group to look up
	///\return the group corresponding to the name, or an invalid group if none exists
	Group getGroup(const std::string& groupName);
	
	GroupRequest getGroupRequest(const std::string& groupName);
	
	bool approveGroupRequest(const std::string& groupName);
	
	///\return whether the attribute was successfully recorded
	bool setGroupSecondaryAttribute(const std::string& groupName, const std::string& attributeName, const std::string& attributeValue);
	
	std::string getGroupSecondaryAttribute(const std::string& groupName, const std::string& attributeName);
	
	///\return whether the attribute was successfully deleted
	bool removeGroupSecondaryAttribute(const std::string& groupName, const std::string& attributeName);
	
	///Return human-readable performance statistics
	std::string getStatistics() const;
	
	const User& getRootUser() const{ return rootUser; }
	
	EmailClient& getEmailClient(){ return emailClient; }
	
private:
	///Database interface object
	Aws::DynamoDB::DynamoDBClient dbClient;
	///Name of the users table in the database
	const std::string userTableName;
	///Name of the groups table in the database
	const std::string groupTableName;
	
	const static unsigned int minimumUserID, maximumUserID;
	const static unsigned int minimumGroupID, maximumGroupID;
	const static std::string nextIDKeyName;
	
	User rootUser;
	
	EmailClient emailClient;
	
	///duration for which cached user records should remain valid
	const std::chrono::seconds userCacheValidity;
	connect_atomic<std::chrono::steady_clock::time_point> userCacheExpirationTime;
	cuckoohash_map<std::string,CacheRecord<User>> userCache;
	cuckoohash_map<std::string,CacheRecord<User>> userByTokenCache;
	cuckoohash_map<std::string,CacheRecord<User>> userByGlobusIDCache;
	///This cache holds secondary user attributes
	cuckoohash_map<std::string,std::map<std::string,CacheRecord<std::string>>> userAttributeCache;
	///This cache holds individual membership records, keyed by userID:groupName
	cuckoohash_map<std::string,CacheRecord<GroupMembership>> groupMembershipCache;
	///This cache holds all memberships associated with each user
	concurrent_multimap<std::string,CacheRecord<GroupMembership>> groupMembershipByUserCache;
	///This cache holds all memberships associated with each group
	concurrent_multimap<std::string,CacheRecord<GroupMembership>> groupMembershipByGroupCache;
	///This cache holds secondary group attributes
	cuckoohash_map<std::string,std::map<std::string,CacheRecord<std::string>>> groupAttributeCache;
	///duration for which cached group records should remain valid
	const std::chrono::seconds groupCacheValidity;
	connect_atomic<std::chrono::steady_clock::time_point> groupCacheExpirationTime;
	cuckoohash_map<std::string,CacheRecord<Group>> groupCache;
	connect_atomic<std::chrono::steady_clock::time_point> groupRequestCacheExpirationTime;
	cuckoohash_map<std::string,CacheRecord<GroupRequest>> groupRequestCache;
	
	///Check that all necessary tables exist in the database, and create them if 
	///they do not
	void InitializeTables(std::string bootstrapUserFile);
	
	void InitializeUserTable();
	void InitializeGroupTable();
	
	///Ensure that a string is a group ID, rather than a group name. 
	///\param groupID the group ID or name. If the value is a valid name, it will 
	///               be replaced with the corresponding ID. 
	///\return true if the ID has been successfully normalized, false if it 
	///        could not be because it was neither a valid group ID nor name. 
	bool normalizeGroupID(std::string& groupID);
	
	///Ensure that a group name is ready to store in dynamo
	std::string encodeGroupName(std::string name);
	///Turn a group name suitable for dynamo back to normal
	std::string decodeGroupName(std::string name);
	
	unsigned int getNextIDHint(const std::string& tableName, const std::string& nameKeyName);
	bool checkIDAvailability(const std::string& tableName, 
	                         const std::string& nameKeyName,
	                         unsigned int id);
	bool reserveUnixID(const std::string& tableName, 
	                   const std::string& nameKeyName,
	                   unsigned int expected,
	                   unsigned int id,
	                   unsigned int next,
	                   const std::string& recordName);
	
	///\param tableName the name of the table in which to allocate the ID
	///\param nameKeyName the name used for the hash key used by the table in 
	///                   which the ID is to be allocated
	///\param minID the minimum allowed ID number in the table, as part of a 
	///                 half-open range
	///\param maxID the maximum allowed ID number in the table, as part of a 
	///                 half-open range
	///\param recordName the name of the record for which the ID will be allocated
	unsigned int allocateUnixID(const std::string& tableName, 
	                            const std::string& nameKeyName, 
	                            const unsigned int minID, 
	                            const unsigned int maxID, 
	                            std::string recordName);
	bool allocateSpecificUnixID(const std::string& tableName, 
	                            const std::string& nameKeyName, 
	                            const unsigned int minID, 
	                            const unsigned int maxID, 
	                            std::string recordName,
	                            unsigned int targetID);
	
	std::atomic<size_t> cacheHits, databaseQueries, databaseScans;
};

///\param store the database in which to look up the user
///\param token the proffered authentication token. May be NULL if missing.
const User authenticateUser(PersistentStore& store, const char* token);

#endif //CONNECT_PERSISTENT_STORE_H
