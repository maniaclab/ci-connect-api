#include "UserCommands.h"

#include "Logging.h"
#include "ServerUtilities.h"
#include "GroupCommands.h"

crow::response listUsers(PersistentStore& store, const crow::request& req){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to list users");
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	//TODO: Are all users are allowed to list all users?

	std::vector<User> users;
	users = store.listUsers();

	rapidjson::Document result(rapidjson::kObjectType);
	rapidjson::Document::AllocatorType& alloc = result.GetAllocator();
	
	result.AddMember("apiVersion", "v1alpha1", alloc);
	rapidjson::Value resultItems(rapidjson::kArrayType);
	resultItems.Reserve(users.size(), alloc);
	for(const User& user : users){
		rapidjson::Value userResult(rapidjson::kObjectType);
		userResult.AddMember("kind", "User", alloc);
		rapidjson::Value userData(rapidjson::kObjectType);
		userData.AddMember("name", rapidjson::StringRef(user.name.c_str()), alloc);
		userData.AddMember("email", rapidjson::StringRef(user.email.c_str()), alloc);
		userData.AddMember("phone", rapidjson::StringRef(user.phone.c_str()), alloc);
		userData.AddMember("institution", rapidjson::StringRef(user.institution.c_str()), alloc);
		userData.AddMember("unix_name", user.unixName, alloc);
		userData.AddMember("join_date", user.joinDate, alloc);
		userData.AddMember("last_use_time", user.lastUseTime, alloc);
		userResult.AddMember("metadata", userData, alloc);
		resultItems.PushBack(userResult, alloc);
	}
	result.AddMember("items", resultItems, alloc);
	
	return crow::response(to_string(result));
}

//namespace{

///Check that a string looks like one or more SSH keys. 
///Note that this does not validate that the key type(s) claimed is(are) valid,
///or that the key data makes any sense. 
///\return true if string's structure appears valid
bool validateSSHKeys(const std::string& keyData){
	const static std::string whitespace=" \t\v"; //not including newlines!
	const static std::string newlineChars="\n\r";
	const static std::string base64Chars="ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	                                     "abcdefghijklmnopqrstuvwxyz"
	                                     "0123456789+/";
	const static char base64Padding='=';
	
	auto isIn=[](const std::string& set, char c)->bool{
		return set.find(c)!=std::string::npos;
	};
	
	bool seenCompleteKey=false;
	enum State{
		LookingForKeyType,
		InKeyType,
		LookingForKeyData,
		InKeyData,
		InKeyDataPadding,
		LookingForCommentOrLineEnd,
		InComment,
	} state=LookingForKeyType;
	
	unsigned int keyDataLen=0, keyPaddingLen=0;
	auto checkBase64Length=[&keyDataLen,&keyPaddingLen](){
		return (keyDataLen+keyPaddingLen)%4==0;
	};
	
	for(const char c : keyData){
		switch(state){
			case LookingForKeyType:
				//Consume whitespace and newlines until something which could be 
				//a key type is found
				if(isIn(newlineChars,c) || isIn(whitespace,c)){
					//ignore, state remains same
				}
				else
					state=InKeyType;
				break;
			case InKeyType:
				//Consume non-whitespace characters until the next whitespace is 
				//reached. Reject newlines. 
				if(isIn(newlineChars,c)){
					//std::cout << "Illegal newline in key type" << std::endl;
					return false; //no newline allowed here
				}
				else if(isIn(whitespace,c))
					state=LookingForKeyData; //end of key type
				//otherwise state remains same
				break;
			case LookingForKeyData:
				//Consume whitespace, but reject newlines. 
				if(isIn(newlineChars,c)){
					//std::cout << "Illegal newline before key data" << std::endl;
					return false; //no newline allowed here
				}
				else if(isIn(whitespace,c)){
					//no state change
				}
				else{
					keyDataLen=1;
					keyPaddingLen=0;
					state=InKeyData;
				}
				break;
			case InKeyData:
				//Consume non-whitespace. 
				if(isIn(base64Chars,c)){
					keyDataLen++;
					//no state change
				}
				else if(c==base64Padding){
					keyPaddingLen=1;
					state=InKeyDataPadding;
				}
				else if(isIn(whitespace,c)){
					if(!checkBase64Length()){
						//std::cout << "Base64 encoding has wrong length (InKeyData->whitespace): " << (keyDataLen+keyPaddingLen) << std::endl;
						return false; //wrong length/padding for base64
					}
					seenCompleteKey=true;
					state=LookingForCommentOrLineEnd;
				}
				else if(isIn(newlineChars,c)){
					if(!checkBase64Length()){
						//std::cout << "Base64 encoding has wrong length (InKeyData->newline): " << (keyDataLen+keyPaddingLen) << std::endl;
						return false; //wrong length/padding for base64
					}
					seenCompleteKey=true;
					state=LookingForKeyType;
				}
				else{ //illegal character
					std::cout << "Illegal character in key data" << std::endl;
					return false;
				}
				break;
			case InKeyDataPadding:
				//Consume padding characters. 
				if(c==base64Padding){
					keyPaddingLen++;
					//no state change
				}
				else if(isIn(whitespace,c)){
					if(!checkBase64Length()){
						//std::cout << "Base64 encoding has wrong length (InKeyDataPadding->whitespace): " << (keyDataLen+keyPaddingLen) << std::endl;
						return false; //wrong length/padding for base64
					}
					seenCompleteKey=true;
					state=LookingForCommentOrLineEnd;
				}
				else if(isIn(newlineChars,c)){
					if(!checkBase64Length()){
						//std::cout << "Base64 encoding has wrong length (InKeyDataPadding->newline): " << (keyDataLen+keyPaddingLen) << std::endl;
						return false; //wrong length/padding for base64
					}
					seenCompleteKey=true;
					state=LookingForKeyType;
				}
				else{ //illegal character
					//std::cout << "Illegal character in key padding" << std::endl;
					return false;
				}
				break;
			case LookingForCommentOrLineEnd:
				if(isIn(whitespace,c)){
					//no state change
				}
				else if(isIn(newlineChars,c))
					state=LookingForKeyType;
				else
					state=InComment;
				break;
			case InComment:
				//Consume everything, waiting for a newline
				if(isIn(newlineChars,c))
					state=LookingForKeyType;
				else{
					//no state change
				}
				break;
		}
	}
	if(!seenCompleteKey && (state==InKeyData || state==InKeyDataPadding)){
		//data ended, but we may have seen a whole, valid key anyway
		if(!checkBase64Length())
			std::cout << "Base64 encoding has wrong length (truncated)" << std::endl;
		return checkBase64Length();
	}
	if(!seenCompleteKey)
		std::cout << "Did not read a complete key entry" << std::endl;
	return seenCompleteKey;
}

//}

crow::response createUser(PersistentStore& store, const crow::request& req){
	//important: user is the user issuing the command, not the user being modified
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to create a user");
	if(!user){
		log_warn(user << " is not authorized to create users");
		return crow::response(403,generateError("Not authorized"));
	}
	
	if(!user.superuser){ //only administrators can create new users
		log_warn(user << " is not a superuser and so is not allowed to create users");
		return crow::response(403,generateError("Not authorized"));
	}
	
	//unpack the target user info
	rapidjson::Document body;
	try{
		body.Parse(req.body.c_str());
	}catch(std::runtime_error& err){
		log_warn("User creation request body was not valid JSON");
		return crow::response(400,generateError("Invalid JSON in request body"));
	}
	
	if(body.IsNull()){
		log_warn("User creation request body was null; raw data:\n" << req.body);
		return crow::response(400,generateError("Invalid JSON in request body"));
	}
	if(!body.HasMember("metadata")){
		log_warn("User creation request body was missing metadata");
		return crow::response(400,generateError("Missing user metadata in request"));
	}
	if(!body["metadata"].IsObject()){
		log_warn("User creation request body was not an object");
		return crow::response(400,generateError("Incorrect type for configuration"));
	}
	
	if(!body["metadata"].HasMember("globusID")){
		log_warn("User creation request was missing globus ID");
		return crow::response(400,generateError("Missing user globus ID in request"));
	}
	if(!body["metadata"]["globusID"].IsString()){
		log_warn("Globus ID in user creation request was not a string");
		return crow::response(400,generateError("Incorrect type for user globus ID"));
	}
	if(!body["metadata"].HasMember("name")){
		log_warn("User creation request was missing user name");
		return crow::response(400,generateError("Missing user name in request"));
	}
	if(!body["metadata"]["name"].IsString()){
		log_warn("User name in user creation request was not a string");
		return crow::response(400,generateError("Incorrect type for user name"));
	}
	if(!body["metadata"].HasMember("email")){
		log_warn("User creation request was missing user email address");
		return crow::response(400,generateError("Missing user email in request"));
	}
	if(!body["metadata"]["email"].IsString()){
		log_warn("User email address in user creation request was not a string");
		return crow::response(400,generateError("Incorrect type for user email"));
	}
	if(!body["metadata"].HasMember("phone")){
		log_warn("User creation request was missing user phone number");
		return crow::response(400,generateError("Missing user phone in request"));
	}
	if(!body["metadata"]["phone"].IsString()){
		log_warn("User phone number in user creation request was not a string");
		return crow::response(400,generateError("Incorrect type for user phone"));
	}
	if(!body["metadata"].HasMember("institution")){
		log_warn("User creation request was missing user institution");
		return crow::response(400,generateError("Missing user institution in request"));
	}
	if(!body["metadata"]["institution"].IsString()){
		log_warn("User institution in user creation request was not a string");
		return crow::response(400,generateError("Incorrect type for user institution"));
	}
	if(body["metadata"].HasMember("public_key") && !body["metadata"]["public_key"].IsString()){
		log_warn("User public key in user creation request was not a string");
		return crow::response(400,generateError("Incorrect type for user public key"));
	}
	if(!body["metadata"].HasMember("unix_name")){
		log_warn("User creation request was missing user unix name");
		return crow::response(400,generateError("Missing user unix name in request"));
	}
	if(!body["metadata"]["unix_name"].IsString()){
		log_warn("User unix name in user creation request was not a string");
		return crow::response(400,generateError("Incorrect type for user unix name key"));
	}
	if(!body["metadata"].HasMember("superuser")){
		log_warn("User creation request was missing superuser flag");
		return crow::response(400,generateError("Missing superuser flag in request"));
	}
	if(!body["metadata"]["superuser"].IsBool()){
		log_warn("Superuser flag in user creation request was not a string");
		return crow::response(400,generateError("Incorrect type for user superuser flag"));
	}
	if(!body["metadata"].HasMember("service_account")){
		log_warn("User creation request was missing service account flag");
		return crow::response(400,generateError("Missing service account flag in request"));
	}
	if(!body["metadata"]["service_account"].IsBool()){
		log_warn("Service account flag in user creation request was not a string");
		return crow::response(400,generateError("Incorrect type for user service account flag"));
	}
	
	User targetUser;
	targetUser.token=idGenerator.generateUserToken();
	targetUser.globusID=body["metadata"]["globusID"].GetString();
	if(targetUser.globusID.empty()){
		log_warn("User globusID was emtpy");
		return crow::response(400,generateError("Empty user Globus ID"));
	}
	targetUser.name=body["metadata"]["name"].GetString();
	if(targetUser.name.empty()){
		log_warn("User name was emtpy");
		return crow::response(400,generateError("Empty user name"));
	}
	targetUser.email=body["metadata"]["email"].GetString();
	if(targetUser.email.empty()){
		log_warn("User email was emtpy");
		return crow::response(400,generateError("Empty user email address"));
	}
	targetUser.phone=body["metadata"]["phone"].GetString();
	if(targetUser.phone.empty()){
		log_warn("User phone was emtpy");
		return crow::response(400,generateError("Empty user phone number"));
	}
	targetUser.institution=body["metadata"]["institution"].GetString();
	if(targetUser.institution.empty()){
		log_warn("User institution was emtpy");
		return crow::response(400,generateError("Empty user institution name"));
	}
	if(body["metadata"].HasMember("public_key")){
		targetUser.sshKey=body["metadata"]["public_key"].GetString();
		if(targetUser.sshKey.empty())
			targetUser.sshKey=" "; //dummy data to keep dynamo happy
		else if(!validateSSHKeys(targetUser.sshKey)){
			log_warn("Malformed SSH key(s)");
			return crow::response(400,generateError("Malformed SSH key(s)"));
		}
	}
	else
		targetUser.sshKey=" "; //dummy data to keep dynamo happy
	targetUser.unixName=body["metadata"]["unix_name"].GetString();
	if(targetUser.unixName.empty()){
		log_warn("User unixName was emtpy");
		return crow::response(400,generateError("Empty user unix account name"));
	}
	targetUser.superuser=body["metadata"]["superuser"].GetBool();
	targetUser.serviceAccount=body["metadata"]["service_account"].GetBool();
	targetUser.joinDate=timestamp();
	targetUser.lastUseTime=targetUser.joinDate;
	targetUser.valid=true;
	
	if(store.findUserByGlobusID(targetUser.globusID)){
		log_warn("User Globus ID is already registered");
		return crow::response(400,generateError("Globus ID is already registered"));
	}
	
	if(store.unixNameInUse(targetUser.unixName)){
		log_warn("User unix name is already in use");
		return crow::response(400,generateError("Unix name is already in use"));
	}
	
	log_info("Creating " << targetUser);
	bool created=store.addUser(targetUser);
	
	if(!created){
		log_error("Failed to create user account");
		return crow::response(500,generateError("User account creation failed"));
	}
	
	GroupMembership baseMembership;
	baseMembership.userName=targetUser.unixName;
	baseMembership.groupName="root"; //the root group
	baseMembership.state=GroupMembership::Active;
	baseMembership.stateSetBy="user:"+store.getRootUser().unixName;
	baseMembership.valid=true;
	if(!store.setUserStatusInGroup(baseMembership))
		log_error("Failed to add new user to root group");

	rapidjson::Document result(rapidjson::kObjectType);
	rapidjson::Document::AllocatorType& alloc = result.GetAllocator();
	
	result.AddMember("apiVersion", "v1alpha1", alloc);
	rapidjson::Value metadata(rapidjson::kObjectType);
	metadata.AddMember("name", targetUser.name, alloc);
	metadata.AddMember("email", targetUser.email, alloc);
	metadata.AddMember("phone", targetUser.phone, alloc);
	metadata.AddMember("institution", targetUser.institution, alloc);
	metadata.AddMember("access_token", targetUser.token, alloc);
	metadata.AddMember("public_key", targetUser.sshKey, alloc);
	metadata.AddMember("join_date", targetUser.joinDate, alloc);
	metadata.AddMember("last_use_time", targetUser.lastUseTime, alloc);
	metadata.AddMember("unix_name", targetUser.unixName, alloc);
	metadata.AddMember("superuser", targetUser.superuser, alloc);
	metadata.AddMember("service_account", targetUser.serviceAccount, alloc);
	rapidjson::Value groupMemberships(rapidjson::kArrayType);
	std::vector<GroupMembership> groupMembershipList = store.getUserGroupMemberships(targetUser.unixName);
	for (auto group : groupMembershipList) {
		rapidjson::Value entry(rapidjson::kObjectType);
		entry.AddMember("name", group.groupName, alloc);
		entry.AddMember("state", GroupMembership::to_string(group.state), alloc);
		entry.AddMember("state_set_by", group.stateSetBy, alloc);
		groupMemberships.PushBack(entry, alloc);
	}
	metadata.AddMember("group_memberships", groupMemberships, alloc);
	result.AddMember("metadata", metadata, alloc);
	
	return crow::response(to_string(result));
}

crow::response getUserInfo(PersistentStore& store, const crow::request& req, const std::string uID){
	//important: user is the user issuing the command, not the user being modified
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested information about " << uID);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	//users can only be examined by admins or themselves
	if(!user.superuser && user.unixName!=uID)
		return crow::response(403,generateError("Not authorized"));
	
	User targetUser=store.getUser(uID);
	if(!targetUser)
		return crow::response(404,generateError("Not found"));

	rapidjson::Document result(rapidjson::kObjectType);
	rapidjson::Document::AllocatorType& alloc = result.GetAllocator();
	
	result.AddMember("apiVersion", "v1alpha1", alloc);
	result.AddMember("kind", "User", alloc);
	rapidjson::Value metadata(rapidjson::kObjectType);
	metadata.AddMember("name", targetUser.name, alloc);
	metadata.AddMember("email", targetUser.email, alloc);
	metadata.AddMember("phone", targetUser.phone, alloc);
	metadata.AddMember("institution", targetUser.institution, alloc);
	metadata.AddMember("access_token", targetUser.token, alloc);
	metadata.AddMember("public_key", targetUser.sshKey, alloc);
	metadata.AddMember("unix_name", targetUser.unixName, alloc);
	metadata.AddMember("join_date", targetUser.joinDate, alloc);
	metadata.AddMember("last_use_time", targetUser.lastUseTime, alloc);
	metadata.AddMember("superuser", targetUser.superuser, alloc);
	metadata.AddMember("service_account", targetUser.serviceAccount, alloc);
	rapidjson::Value groupMemberships(rapidjson::kArrayType);
	std::vector<GroupMembership> groupMembershipList = store.getUserGroupMemberships(uID);
	for (auto group : groupMembershipList) {
		rapidjson::Value entry(rapidjson::kObjectType);
		entry.AddMember("name", group.groupName, alloc);
		entry.AddMember("state", GroupMembership::to_string(group.state), alloc);
		entry.AddMember("state_set_by", group.stateSetBy, alloc);
		groupMemberships.PushBack(entry, alloc);
	}
	metadata.AddMember("group_memberships", groupMemberships, alloc);
	result.AddMember("metadata", metadata, alloc);
	
	return crow::response(to_string(result));
}

crow::response updateUser(PersistentStore& store, const crow::request& req, const std::string uID){
	//important: user is the user issuing the command, not the user being modified
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to update information about " << uID);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	//users can only be altered by admins and themselves
	if(!user.superuser && user.unixName!=uID)
		return crow::response(403,generateError("Not authorized"));
	
	User targetUser=store.getUser(uID);
	
	if(!targetUser)
		return crow::response(404,generateError("User not found"));
	
	//unpack the target user info
	rapidjson::Document body;
	try{
		body.Parse(req.body.c_str());
	}catch(std::runtime_error& err){
		return crow::response(400,generateError("Invalid JSON in request body"));
	}
	if(body.IsNull())
		return crow::response(400,generateError("Invalid JSON in request body"));
	if(!body.HasMember("metadata"))
		return crow::response(400,generateError("Missing user metadata in request"));
	if(!body["metadata"].IsObject())
		return crow::response(400,generateError("Incorrect type for user metadata"));
	
	User updatedUser=targetUser;
	
	if(body["metadata"].HasMember("name")){
		if(!body["metadata"]["name"].IsString())
			return crow::response(400,generateError("Incorrect type for user name"));
		updatedUser.name=body["metadata"]["name"].GetString();
	}
	if(body["metadata"].HasMember("email")){
		if(!body["metadata"]["email"].IsString())
			return crow::response(400,generateError("Incorrect type for user email"));
		updatedUser.email=body["metadata"]["email"].GetString();
	}
	if(body["metadata"].HasMember("phone")){
		if(!body["metadata"]["phone"].IsString())
			return crow::response(400,generateError("Incorrect type for user phone"));
		updatedUser.phone=body["metadata"]["phone"].GetString();
	}
	if(body["metadata"].HasMember("institution")){
		if(!body["metadata"]["institution"].IsString())
			return crow::response(400,generateError("Incorrect type for user institution"));
		updatedUser.institution=body["metadata"]["institution"].GetString();
	}
	if(body["metadata"].HasMember("public_key")){
		if(!body["metadata"]["public_key"].IsString())
			return crow::response(400,generateError("Incorrect type for user public key"));
		updatedUser.sshKey=body["metadata"]["public_key"].GetString();
		if(updatedUser.sshKey.empty())
			updatedUser.sshKey=" ";
		else if(!validateSSHKeys(updatedUser.sshKey)){
			log_warn("Malformed SSH key(s)");
			return crow::response(400,generateError("Malformed SSH key(s)"));
		}
	}
	if(body["metadata"].HasMember("superuser")){
		if(!body["metadata"]["superuser"].IsBool())
			return crow::response(400,generateError("Incorrect type for user superuser flag"));
		if(!user.superuser && body["metadata"]["superuser"].GetBool()!=targetUser.superuser) //only admins can alter admin rights
			return crow::response(403,generateError("Not authorized"));
		if(user.superuser)
			updatedUser.superuser=body["metadata"]["superuser"].GetBool();
	}
	if(body["metadata"].HasMember("globusID")){
		if(!body["metadata"]["globusID"].IsString())
			return crow::response(400,generateError("Incorrect type for user globus ID"));
		updatedUser.globusID=body["metadata"]["globusID"].GetString();
	}
	
	log_info("Updating " << targetUser << " info");
	bool updated=store.updateUser(updatedUser,targetUser);
	
	if(!updated)
		return crow::response(500,generateError("User account update failed"));
	return(crow::response(200));
}

crow::response deleteUser(PersistentStore& store, const crow::request& req, const std::string uID){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " to delete " << uID);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	//users can only be deleted by admins or themselves
	if(!user.superuser && user.unixName!=uID)
		return crow::response(403,generateError("Not authorized"));
	
	User targetUser;
	if(user.unixName==uID)
		targetUser=user;
	else{
		targetUser=store.getUser(uID);
		if(!targetUser)
			return crow::response(404,generateError("Not found"));
	}
	
	log_info("Deleting " << targetUser);
	//Remove the user from any groups
	std::vector<GroupMembership> groupMembershipList = store.getUserGroupMemberships(uID);
	for(auto& membership : groupMembershipList)
		store.removeUserFromGroup(uID,membership.groupName);
	bool deleted=store.removeUser(uID);
	
	if(!deleted)
		return crow::response(500,generateError("User account deletion failed"));
	
	//send email notification
	EmailClient::Email message;
	message.fromAddress="no-reply@ci-connect.net";
	message.toAddresses={targetUser.email};
	message.subject="CI-Connect account deleted";
	message.body="This is an automatic notification that your CI-Connect user "
	"account ("+targetUser.unixName+") has been deleted";
	if(user!=targetUser)
		message.body+=" by "+user.name;
	message.body+=".";
	store.getEmailClient().sendEmail(message);	
	
	return(crow::response(200));
}

crow::response listUserGroups(PersistentStore& store, const crow::request& req, const std::string uID){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested Group listing for " << uID);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	
	User targetUser;
	if(user.unixName==uID)
		targetUser=user;
	else{
		User targetUser=store.getUser(uID);
		if(!targetUser)
			return crow::response(404,generateError("Not found"));
	}
	//TODO: can anyone list anyone else's Group memberships?

	rapidjson::Document result(rapidjson::kObjectType);
	rapidjson::Document::AllocatorType& alloc = result.GetAllocator();
	
	result.AddMember("apiVersion", "v1alpha1", alloc);
	rapidjson::Value groupMemberships(rapidjson::kArrayType);
	std::vector<GroupMembership> groupMembershipList = store.getUserGroupMemberships(uID);
	for (auto membership : groupMembershipList) {
		rapidjson::Value entry(rapidjson::kObjectType);
		entry.AddMember("name", membership.groupName, alloc);
		entry.AddMember("state", GroupMembership::to_string(membership.state), alloc);
		entry.AddMember("state_set_by", membership.stateSetBy, alloc);
		groupMemberships.PushBack(entry, alloc);
	}
	result.AddMember("group_memberships", groupMemberships, alloc);

	return crow::response(to_string(result));
}

namespace{
	///\return the name of most closely enclosing group of which the user is an admin
	std::string adminInAnyEnclosingGroup(PersistentStore& store, const std::string& userID, std::string groupName){
		while(!groupName.empty()){
			auto sepPos=groupName.rfind('.');
			if(sepPos==std::string::npos)
				return ""; //no farther up to walk
			groupName=groupName.substr(0,sepPos);
			if(groupName.empty())
				return "";
			if(store.userStatusInGroup(userID,groupName).state==GroupMembership::Admin)
				return groupName;
		}
		return "";
	}
	
	///Make sure that the given user is a member of every group enclosing the 
	///specified one, and if not, add them. This bypasses pending states, etc.
	///so it should only be applied when the user becomes an active member, not
	///pending or disabled. 
	/*bool ensureEnclosingMembership(PersistentStore& store, const std::string& userID, std::string groupName, const std::string& stateSetBy){
		while(!groupName.empty()){
			auto sepPos=groupName.rfind('.');
			if(sepPos==std::string::npos)
				return true; //no farther up to walk
			groupName=groupName.substr(0,sepPos);
			if(groupName.empty())
				return true;
			
			if(!store.userStatusInGroup(userID,groupName).isMember()){
				//is the user is not already a member at this level, add them
				GroupMembership membership;
				membership.userName=userID;
				membership.groupName=groupName;
				membership.stateSetBy=stateSetBy;
				membership.state=GroupMembership::Active; //always as a normal member
				if(!store.setUserStatusInGroup(membership)){
					log_error("Failed to add user " << userID << " to " << groupName << "; enclosing group memberships may be inconsistent");
					return false;
				}
			}
		}
		return true;
	}*/
}

crow::response setUserStatusInGroup(PersistentStore& store, const crow::request& req, 
						   const std::string& uID, std::string groupName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to add " << uID << " to " << groupName);
	if(!user){
		log_warn(user << " does not exist");
		return crow::response(403,generateError("Not authorized"));
	}
	
	User targetUser=store.getUser(uID);
	if(!targetUser){
		log_warn(targetUser << " does not exist");
		return crow::response(404,generateError("User not found"));
	}
	
	groupName=canonicalizeGroupName(groupName);
	Group group=store.getGroup(groupName);
	if(!group){
		log_warn(group << " does not exist");
		return(crow::response(404,generateError("Group not found")));
	}
		
	rapidjson::Document body;
	try{
		body.Parse(req.body.c_str());
	}catch(std::runtime_error& err){
		return crow::response(400,generateError("Invalid JSON in request body"));
	}
	if(body.IsNull())
		return crow::response(400,generateError("Invalid JSON in request body"));
	if(!body.HasMember("group_membership"))
		return crow::response(400,generateError("Missing group_membership in request"));
	if(!body["group_membership"].IsObject())
		return crow::response(400,generateError("Incorrect type for group_membership"));
	
	GroupMembership membership;
	membership.userName=targetUser.unixName;
	membership.groupName=group.name;
	membership.stateSetBy="user:"+user.unixName;
	
	if(body["group_membership"].HasMember("state")){
		if(!body["group_membership"]["state"].IsString())
			return crow::response(400,generateError("Incorrect type for membership state"));
		membership.state=GroupMembership::from_string(body["group_membership"]["state"].GetString());
	}
	
	auto currentStatus=store.userStatusInGroup(targetUser.unixName,group.name);
	if(membership.state==currentStatus.state) //no-op
		return(crow::response(200));
	bool selfRequest=(user==targetUser);
	bool requesterIsGroupAdmin=(store.userStatusInGroup(user.unixName,groupName).state==GroupMembership::Admin);
	bool requesterIsEnclosingGroupAdmin=false;
	std::string adminGroup;
	if(requesterIsGroupAdmin)
		adminGroup=groupName;
	else{
		adminGroup=adminInAnyEnclosingGroup(store,user.unixName,group.name);
		if(!adminGroup.empty())
			requesterIsEnclosingGroupAdmin=true;
	}
	
	//check whether the target user belongs to the enclosing group
	std::string enclosingGroupName=enclosingGroup(group.name);
	if(enclosingGroupName!=group.name &&
	   !store.userStatusInGroup(targetUser.unixName,enclosingGroupName).isMember())
		return crow::response(400,generateError("Cannot modify user status in group: Target user is not a member of the enclosing group"));
	
	//Figure out whether the requested transition is allowed
	switch(membership.state){
		case GroupMembership::NonMember:
			return crow::response(400,generateError("User status cannot be explicitly set to non-member"));
		case GroupMembership::Pending:
			if(currentStatus.state!=GroupMembership::NonMember)
				return crow::response(400,generateError("Only non-members can be placed in pending membership status"));
			//if(!user.superuser && !requesterIsGroupAdmin && !requesterIsEnclosingGroupAdmin)
			//	return crow::response(403,generateError("Not authorized"));
			break; //allowed
		case GroupMembership::Active: //fallthrough
		case GroupMembership::Admin:
			if(currentStatus.state==GroupMembership::Disabled){
				if(!user.superuser && !requesterIsGroupAdmin)
					return crow::response(403,generateError("Not authorized"));
				break; //allowed
			}
			else{
				if(!user.superuser && !requesterIsGroupAdmin &&!requesterIsEnclosingGroupAdmin)
					return crow::response(403,generateError("Not authorized"));
				break; //allowed
			}
		case GroupMembership::Disabled:
			if(currentStatus.state==GroupMembership::NonMember ||
			   currentStatus.state==GroupMembership::Pending)
				return crow::response(400,generateError("Only members can be placed in disabled membership status"));
			if(!user.superuser && !requesterIsGroupAdmin && !requesterIsEnclosingGroupAdmin)
				return crow::response(403,generateError("Not authorized"));
			membership.stateSetBy="group:"+adminGroup;
			break; //allowed
	}
	
	log_info("Setting " << targetUser << " status in " << groupName << " to " << GroupMembership::to_string(membership.state));
	
	membership.valid=true;
	bool success=store.setUserStatusInGroup(membership);
	if(!success)
		return crow::response(500,generateError("User addition to Group failed"));
	
	//if(membership.isMember())
	//	ensureEnclosingMembership(store,membership.userName,membership.groupName,membership.stateSetBy);	
	
	//If the user is requesting to join a group, notify the group admins. 
	if(currentStatus.state==GroupMembership::NonMember && membership.state==GroupMembership::Pending){
		EmailClient::Email message;
		message.fromAddress="no-reply@ci-connect.net";
		message.toAddresses={group.email};
		message.ccAddresses={targetUser.email};
		message.subject="CI-Connect group membership request";
		message.body="This is an automatic notification that "+targetUser.name+
		" ("+targetUser.unixName+") has requested to join the "+group.displayName+" group.";
		store.getEmailClient().sendEmail(message);
	}
	else{ //otherwise just inform the user
		EmailClient::Email message;
		message.fromAddress="no-reply@ci-connect.net";
		message.toAddresses={targetUser.email};
		message.subject="CI-Connect group membership change";
		message.body="This is an automatic notification that your membership in the "+
		group.displayName+" group has been set to \""+GroupMembership::to_string(membership.state)+"\".";
		store.getEmailClient().sendEmail(message);
	}
	
	return(crow::response(200));
}

crow::response removeUserFromGroup(PersistentStore& store, const crow::request& req, 
								   const std::string& uID, std::string groupID){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to remove " << uID << " from " << groupID);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	
	User targetUser=store.getUser(uID);
	if(!targetUser)
		return crow::response(404,generateError("User not found"));
	
	groupID=canonicalizeGroupName(groupID);
	
	//Only allow superusers and admins of the Group to remove user from it
	if(!user.superuser && 
	   store.userStatusInGroup(user.unixName,groupID).state!=GroupMembership::Admin &&
	   adminInAnyEnclosingGroup(store,user.unixName,groupID).empty())
		return crow::response(403,generateError("Not authorized"));
	
	auto currentStatus=store.userStatusInGroup(targetUser.unixName,groupID);
	
	log_info("Removing " << targetUser << " from " << groupID);
	//first, remove the user from any subgroups of the group in question
	std::vector<GroupMembership> memberships = store.getUserGroupMemberships(uID);
	for(const auto& membership : memberships){
		//find all memberships within the target group
		if(membership.groupName.find(groupID)==0){
			if(membership.groupName==groupID) //exact match for the target group
				continue; //we'll come back to this one
			log_info("Removing " << targetUser << " from subgroup " << membership.groupName);
			bool success=store.removeUserFromGroup(uID,membership.groupName);
			if(!success)
				return crow::response(500,generateError("User removal from Group failed"));
		}
	}
	//finally, do the removal from the target group
	bool success=store.removeUserFromGroup(uID,groupID);
	if(!success)
		return crow::response(500,generateError("User removal from Group failed"));
		
	if(currentStatus.state==GroupMembership::Pending){
		EmailClient::Email message;
		message.fromAddress="no-reply@ci-connect.net";
		message.toAddresses={targetUser.email};
		message.subject="CI-Connect group membership request denied";
		message.body="This is an automatic notification that your request to join the "+
		groupID+" group has been denied by the group administrators.";
		store.getEmailClient().sendEmail(message);
	}
	else{
		EmailClient::Email message;
		message.fromAddress="no-reply@ci-connect.net";
		message.toAddresses={targetUser.email};
		message.subject="CI-Connect group membership change";
		message.body="This is an automatic notification that your account has been removed from the "+
		groupID+" group.";
		store.getEmailClient().sendEmail(message);
	}
	
	return(crow::response(200));
}

crow::response getUserAttribute(PersistentStore& store, const crow::request& req, 
                                std::string uID, std::string attributeName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to fetch secondary attribute " << attributeName << " of user " << uID);
	if(!user)
		return crow::response(403,generateError("Not authorized"));

	//Any user can query any user property?
	
	std::string value=store.getUserSecondaryAttribute(uID, attributeName);
	if(value.empty())
		return crow::response(404,generateError("User or attribute not found"));
	
	rapidjson::Document result(rapidjson::kObjectType);
	rapidjson::Document::AllocatorType& alloc = result.GetAllocator();
	
	result.AddMember("apiVersion", "v1alpha1", alloc);
	result.AddMember("data", value, alloc);
	
	return crow::response(to_string(result));
}

crow::response setUserAttribute(PersistentStore& store, const crow::request& req, 
                                std::string uID, std::string attributeName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to set secondary attribute " << attributeName << " for user " << uID);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	
	//Only superusers and users can alter themselves
	if(!user.superuser && user.unixName!=uID)
		return crow::response(403,generateError("Not authorized"));

	rapidjson::Document body;
	try{
		body.Parse(req.body.c_str());
	}catch(std::runtime_error& err){
		return crow::response(400,generateError("Invalid JSON in request body"));
	}

	if(body.IsNull())
		return crow::response(400,generateError("Invalid JSON in request body"));
	if(!body.HasMember("data"))
		return crow::response(400,generateError("Missing attribute data in request"));
	if(!body["data"].IsString())
		return crow::response(400,generateError("Attribute data must be a string"));
		
	std::string attributeValue=body["data"].GetString();
	bool success=store.setUserSecondaryAttribute(uID, attributeName, attributeValue);
	
	if(!success)
		return crow::response(500,generateError("Failed to store user attribute"));
	
	return crow::response(200);
}

crow::response deleteUserAttribute(PersistentStore& store, const crow::request& req,
                                   std::string uID, std::string attributeName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to delete secondary attribute " << attributeName << " from user " << uID);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	
	//Only superusers and users can alter themselves
	if(!user.superuser && user.unixName!=uID)
		return crow::response(403,generateError("Not authorized"));
	
	bool success=store.removeUserSecondaryAttribute(uID, attributeName);
	
	if(!success)
		return crow::response(500,generateError("Failed to delete user attribute"));
	
	return crow::response(200);
}

crow::response findUser(PersistentStore& store, const crow::request& req){
	//this is the requesting user, not the requested user
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested user information for a globus ID");
	if(!user || !user.superuser)
		return crow::response(403,generateError("Not authorized"));
	
	if(!req.url_params.get("globus_id"))
		return crow::response(400,generateError("Missing globus ID in request"));
	std::string globusID=req.url_params.get("globus_id");
	
	User targetUser=store.findUserByGlobusID(globusID);
	
	if(!targetUser)
		return crow::response(404,generateError("User not found"));

	rapidjson::Document result(rapidjson::kObjectType);
	rapidjson::Document::AllocatorType& alloc = result.GetAllocator();
	
	result.AddMember("apiVersion", "v1alpha1", alloc);
	result.AddMember("kind", "User", alloc);
	rapidjson::Value metadata(rapidjson::kObjectType);
	metadata.AddMember("unix_name", targetUser.unixName, alloc);
	metadata.AddMember("access_token", targetUser.token, alloc);
	result.AddMember("metadata", metadata, alloc);

	return crow::response(to_string(result));
}

crow::response checkUnixName(PersistentStore& store, const crow::request& req){
	//this is the requesting user
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested whether a unix name is in use");
	if(!user)
		return crow::response(403,generateError("Not authorized"));
		
	if(!req.url_params.get("unix_name"))
		return crow::response(400,generateError("Missing unix name in request"));
	std::string unixName=req.url_params.get("unix_name");
	
	try{
		bool inUse=store.unixNameInUse(unixName);
		rapidjson::Document result(rapidjson::kObjectType);
		rapidjson::Document::AllocatorType& alloc = result.GetAllocator();
		result.AddMember("in_use",inUse,alloc);
		return crow::response(to_string(result));
	}
	catch(std::runtime_error& err){
		return crow::response(500,generateError("Failed to look up unix name"));
	}
}

crow::response replaceUserToken(PersistentStore& store, const crow::request& req, const std::string uID){
	//important: user is the user issuing the command, not the user being modified
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to replace access token for " << uID);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	//users can only be altered by admins and themselves
	if(!user.superuser && user.unixName!=uID)
		return crow::response(403,generateError("Not authorized"));
	
	User targetUser=store.getUser(uID);
	
	if(!targetUser)
		return crow::response(404,generateError("User not found"));
	
	log_info("Updating " << targetUser << " access token");
	User updatedUser=targetUser;
	updatedUser.token=idGenerator.generateUserToken();
	bool updated=store.updateUser(updatedUser,targetUser);
	
	if(!updated)
		return crow::response(500,generateError("User account update failed"));
	
	rapidjson::Document result(rapidjson::kObjectType);
	rapidjson::Document::AllocatorType& alloc = result.GetAllocator();
	
	result.AddMember("apiVersion", "v1alpha1", alloc);
	result.AddMember("kind", "User", alloc);
	rapidjson::Value metadata(rapidjson::kObjectType);
	metadata.AddMember("unix_name", updatedUser.unixName, alloc);
	metadata.AddMember("access_token", updatedUser.token, alloc);
	result.AddMember("metadata", metadata, alloc);
	
	return crow::response(to_string(result));
}

crow::response updateLastUseTime(PersistentStore& store, const crow::request& req, const std::string uID){
	//important: user is the user issuing the command, not the user being modified
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to update last use time for " << uID);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	//users can only be altered by admins and themselves
	if(!user.superuser && user.unixName!=uID)
		return crow::response(403,generateError("Not authorized"));
	
	User targetUser=store.getUser(uID);
	
	if(!targetUser)
		return crow::response(404,generateError("User not found"));
	
	User updatedUser=targetUser;
	updatedUser.lastUseTime=timestamp();
	log_info("Updating " << updatedUser << " last use time to " << updatedUser.lastUseTime);
	bool updated=store.updateUser(updatedUser,targetUser);
	
	if(!updated)
		return crow::response(500,generateError("User account update failed"));
	return crow::response(200);
}
