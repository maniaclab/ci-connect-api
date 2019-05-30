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
		userResult.AddMember("apiVersion", "v1alpha1", alloc);
		userResult.AddMember("kind", "User", alloc);
		rapidjson::Value userData(rapidjson::kObjectType);
		userData.AddMember("id", rapidjson::StringRef(user.id.c_str()), alloc);
		userData.AddMember("name", rapidjson::StringRef(user.name.c_str()), alloc);
		userData.AddMember("email", rapidjson::StringRef(user.email.c_str()), alloc);
		userData.AddMember("phone", rapidjson::StringRef(user.phone.c_str()), alloc);
		userData.AddMember("institution", rapidjson::StringRef(user.institution.c_str()), alloc);
		userResult.AddMember("metadata", userData, alloc);
		resultItems.PushBack(userResult, alloc);
	}
	result.AddMember("items", resultItems, alloc);
	
	return crow::response(to_string(result));
}

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
		log_warn("User creation request body was null");
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
	if(!body["metadata"].HasMember("public_key")){
		log_warn("User creation request was missing user public key");
		return crow::response(400,generateError("Missing user public key in request"));
	}
	if(!body["metadata"]["public_key"].IsString()){
		log_warn("User public key in user creation request was not a string");
		return crow::response(400,generateError("Incorrect type for user public key"));
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
	targetUser.id=idGenerator.generateUserID();
	targetUser.token=idGenerator.generateUserToken();
	targetUser.globusID=body["metadata"]["globusID"].GetString();
	targetUser.name=body["metadata"]["name"].GetString();
	targetUser.email=body["metadata"]["email"].GetString();
	targetUser.phone=body["metadata"]["phone"].GetString();
	targetUser.institution=body["metadata"]["institution"].GetString();
	targetUser.sshKey=body["metadata"]["public_key"].GetString();
	targetUser.superuser=body["metadata"]["superuser"].GetBool();
	targetUser.serviceAccount=body["metadata"]["service_account"].GetBool();
	targetUser.valid=true;
	
	if(store.findUserByGlobusID(targetUser.globusID)){
		log_warn("User Globus ID is already registered");
		return crow::response(400,generateError("Globus ID is already registered"));
	}
	
	log_info("Creating " << targetUser);
	bool created=store.addUser(targetUser);
	
	if(!created){
		log_error("Failed to create user account");
		return crow::response(500,generateError("User account creation failed"));
	}
	
	GroupMembership baseMembership;
	baseMembership.userID=targetUser.id;
	baseMembership.groupName=""; //the root group
	baseMembership.state=GroupMembership::Active;
	baseMembership.stateSetBy=store.getRootUser().id;
	baseMembership.valid=true;
	if(!store.setUserStatusInGroup(baseMembership))
		log_error("Failed to add new user to root group");

	rapidjson::Document result(rapidjson::kObjectType);
	rapidjson::Document::AllocatorType& alloc = result.GetAllocator();
	
	result.AddMember("apiVersion", "v1alpha1", alloc);
	rapidjson::Value metadata(rapidjson::kObjectType);
	metadata.AddMember("id", rapidjson::StringRef(targetUser.id.c_str()), alloc);
	metadata.AddMember("name", rapidjson::StringRef(targetUser.name.c_str()), alloc);
	metadata.AddMember("email", rapidjson::StringRef(targetUser.email.c_str()), alloc);
	metadata.AddMember("phone", rapidjson::StringRef(targetUser.phone.c_str()), alloc);
	metadata.AddMember("institution", rapidjson::StringRef(targetUser.institution.c_str()), alloc);
	metadata.AddMember("access_token", rapidjson::StringRef(targetUser.token.c_str()), alloc);
	metadata.AddMember("public_key", rapidjson::StringRef(targetUser.sshKey.c_str()), alloc);
	metadata.AddMember("superuser", targetUser.superuser, alloc);
	metadata.AddMember("service_account", targetUser.serviceAccount, alloc);
	rapidjson::Value groupMemberships(rapidjson::kArrayType);
	std::vector<GroupMembership> groupMembershipList = store.getUserGroupMemberships(targetUser.id);
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
	if(!user.superuser && user.id!=uID)
		return crow::response(403,generateError("Not authorized"));
	
	User targetUser=store.getUser(uID);
	if(!targetUser)
		return crow::response(404,generateError("Not found"));

	rapidjson::Document result(rapidjson::kObjectType);
	rapidjson::Document::AllocatorType& alloc = result.GetAllocator();
	
	result.AddMember("apiVersion", "v1alpha1", alloc);
	result.AddMember("kind", "User", alloc);
	rapidjson::Value metadata(rapidjson::kObjectType);
	metadata.AddMember("id", rapidjson::StringRef(targetUser.id.c_str()), alloc);
	metadata.AddMember("name", rapidjson::StringRef(targetUser.name.c_str()), alloc);
	metadata.AddMember("email", rapidjson::StringRef(targetUser.email.c_str()), alloc);
	metadata.AddMember("phone", rapidjson::StringRef(targetUser.phone.c_str()), alloc);
	metadata.AddMember("institution", rapidjson::StringRef(targetUser.institution.c_str()), alloc);
	metadata.AddMember("access_token", rapidjson::StringRef(targetUser.token.c_str()), alloc);
	metadata.AddMember("public_key", rapidjson::StringRef(targetUser.sshKey.c_str()), alloc);
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
	if(!user.superuser && user.id!=uID)
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
	if(body["metadata"].HasMember("superuser")){
		if(!body["metadata"]["superuser"].IsBool())
			return crow::response(400,generateError("Incorrect type for user superuser flag"));
		if(!user.superuser && body["metadata"]["superuser"].GetBool()!=targetUser.superuser) //only admins can alter admin rights
			return crow::response(403,generateError("Not authorized"));
		if(user.superuser)
			updatedUser.superuser=body["metadata"]["superuser"].GetBool();
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
	if(!user.superuser && user.id!=uID)
		return crow::response(403,generateError("Not authorized"));
	
	User targetUser;
	if(user.id==uID)
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
	return(crow::response(200));
}

crow::response listUserGroups(PersistentStore& store, const crow::request& req, const std::string uID){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested Group listing for " << uID);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	
	User targetUser;
	if(user.id==uID)
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
}

crow::response setUserStatusInGroup(PersistentStore& store, const crow::request& req, 
						   const std::string& uID, std::string groupName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to add " << uID << " to " << groupName);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	
	User targetUser=store.getUser(uID);
	if(!targetUser)
		return crow::response(404,generateError("User not found"));
	
	groupName=normalizeGroupName(groupName);
	Group group=store.getGroup(groupName);
	if(!group)
		return(crow::response(404,generateError("Group not found")));
		
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
	membership.userID=targetUser.id;
	membership.groupName=group.name;
	membership.stateSetBy=user.id;
	
	if(body["group_membership"].HasMember("state")){
		if(!body["group_membership"]["state"].IsString())
			return crow::response(400,generateError("Incorrect type for membership state"));
		membership.state=GroupMembership::from_string(body["group_membership"]["state"].GetString());
	}
	
	auto currentStatus=store.userStatusInGroup(targetUser.id,group.name);
	if(membership.state==currentStatus.state) //no-op
		return(crow::response(200));
	bool selfRequest=(user==targetUser);
	bool requesterIsGroupAdmin=(store.userStatusInGroup(user.id,groupName).state==GroupMembership::Admin);
	bool requesterIsEnclosingGroupAdmin=false;
	std::string adminGroup;
	if(requesterIsGroupAdmin)
		adminGroup=groupName;
	else{
		adminGroup=adminInAnyEnclosingGroup(store,user.id,group.name);
		if(!adminGroup.empty())
			requesterIsEnclosingGroupAdmin=true;
	}
	
	//Figure out whether the requested transition is allowed
	switch(membership.state){
		case GroupMembership::NonMember:
			return crow::response(400,generateError("Only status cannot be set to non-member"));
		case GroupMembership::Pending:
			if(currentStatus.state!=GroupMembership::NonMember)
				return crow::response(400,generateError("Only non-members can be placed in pending membership status"));
			if(!user.superuser && !requesterIsGroupAdmin && !requesterIsEnclosingGroupAdmin)
				return crow::response(403,generateError("Not authorized"));
			break; //allowed
		case GroupMembership::Active: //fallthrough
		case GroupMembership::Admin:
			if(currentStatus.state==GroupMembership::Disabled){
				if(!user.superuser && !requesterIsGroupAdmin)
					return crow::response(403,generateError("Not authorized"));
				break; //allowed
			}
			else{
				if(!user.superuser && !requesterIsGroupAdmin)
					return crow::response(403,generateError("Not authorized"));
				break; //allowed
			}
		case GroupMembership::Disabled:
			if(currentStatus.state==GroupMembership::NonMember ||
			   currentStatus.state==GroupMembership::Pending)
				return crow::response(400,generateError("Only members can be placed in disabled membership status"));
			if(!user.superuser && !requesterIsGroupAdmin && !requesterIsEnclosingGroupAdmin)
				return crow::response(403,generateError("Not authorized"));
			membership.stateSetBy=adminGroup;
			break; //allowed
	}
	
	log_info("Setting " << targetUser << " status in " << groupName << " to " << GroupMembership::to_string(membership.state));
	
	membership.valid=true;
	bool success=store.setUserStatusInGroup(membership);
	
	if(!success)
		return crow::response(500,generateError("User addition to Group failed"));
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
	
	//Only allow superusers and admins of the Group to remove user from it
	if(!user.superuser && 
	   store.userStatusInGroup(user.id,groupID).state!=GroupMembership::Admin &&
	   adminInAnyEnclosingGroup(store,user.id,groupID).empty())
		return crow::response(403,generateError("Not authorized"));
	
	log_info("Removing " << targetUser << " from " << groupID);
	bool success=store.removeUserFromGroup(uID,groupID);
	
	if(!success)
		return crow::response(500,generateError("User removal from Group failed"));
	return(crow::response(200));
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
	metadata.AddMember("id", rapidjson::StringRef(targetUser.id.c_str()), alloc);
	metadata.AddMember("access_token", rapidjson::StringRef(targetUser.token.c_str()), alloc);
	result.AddMember("metadata", metadata, alloc);

	return crow::response(to_string(result));
}

crow::response replaceUserToken(PersistentStore& store, const crow::request& req, const std::string uID){
	//important: user is the user issuing the command, not the user being modified
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to replace access token for " << uID);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	//users can only be altered by admins and themselves
	if(!user.superuser && user.id!=uID)
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
	metadata.AddMember("id", updatedUser.id, alloc);
	metadata.AddMember("access_token", updatedUser.token, alloc);
	result.AddMember("metadata", metadata, alloc);
	
	return crow::response(to_string(result));
}
