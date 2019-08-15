#include "GroupCommands.h"

#include <boost/lexical_cast.hpp>

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

#include "Logging.h"
#include "ServerUtilities.h"
#include "server_version.h"

namespace{
	const char* scienceFields[]={
		"Resource Provider",
		"Astronomy",
		"Astrophysics",
		"Biology",
		"Biochemistry",
		"Bioinformatics",
		"Biomedical research",
		"Biophysics",
		"Botany",
		"Cellular Biology",
		"Ecology",
		"Evolutionary Biology",
		"Microbiology",
		"Molecular Biology",
		"Neuroscience",
		"Physiology",
		"Structural Biology",
		"Zoology",
		"Chemistry",
		"Biochemistry",
		"Physical Chemistry",
		"Earth Sciences",
		"Economics",
		"Education",
		"Educational Psychology",
		"Engineering",
		"Electronic Engineering",
		"Nanoelectronics",
		"Mathematics & Computer Science",
		"Computer Science",
		"Geographic Information Science",
		"Information Theory",
		"Mathematics",
		"Medicine",
		"Medical Imaging",
		"Neuroscience",
		"Physiology",
		"Logic",
		"Statistics",
		"Physics",
		"Accelerator Physics",
		"Astro-particle Physics",
		"Astrophysics",
		"Biophysics",
		"Computational Condensed Matter Physics",
		"Gravitational Physics",
		"High Energy Physics",
		"Neutrino Physics",
		"Nuclear Physics",
		"Physical Chemistry",
		"Psychology",
		"Child Psychology",
		"Educational Psychology",
		"Materials Science",
		"Multidisciplinary",
		"Network Science",
		"Technology",
	};
	
	///Normalizes a possible field of science string to the matching value in
	///the official list, or returns an empty string if matching failed. 
	std::string normalizeScienceField(const std::string& raw){
		//Use a dumb linear scan so we don't need top worry about the ;ist being 
		//ordered. This isn't very efficient, but also shouldn't be called very 
		//often.
		for(const std::string field : scienceFields){
			if(field.size()!=raw.size())
				continue;
			bool match=true;
			//simple-minded case-insensitive compare.
			//TODO: will this break horribly on non-ASCII UTF-8?
			for(std::size_t i=0; i<field.size(); i++){
				if(std::tolower(raw[i])!=std::tolower(field[i])){
					match=false;
					break;
				}
			}
			if(match)
				return field;
		}
		return "";
	}
}
	
std::string canonicalizeGroupName(std::string name, const std::string& enclosingGroup){
	if(name=="root") //the root group
		return name;
	if(name.find("root.")==0) //assume anything starting with the root group is in absolute form
		return name;
	//otherwise treat as a relative name
	return enclosingGroup+"."+name;
}

///\return baz from root.foo.bar.baz
std::string lastGroupComponent(const std::string& groupName){
	auto pos=groupName.rfind('.');
	if(pos==std::string::npos || pos==groupName.size()-1) //no dots so keep everything
		return groupName;
	return groupName.substr(pos+1);
}

///\pre groupName should be in canonical form
std::string enclosingGroup(const std::string& groupName){
	auto pos=groupName.rfind('.');
	if(pos==std::string::npos || pos==groupName.size()-1) //no dots so keep everything
		return groupName;
	return groupName.substr(0,pos);
}

crow::response listGroups(PersistentStore& store, const crow::request& req){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to list groups");
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	//All users are allowed to list groups

	std::vector<Group> vos;

	//if (req.url_params.get("user"))
	//	vos=store.listgroupsForUser(user.id);
	//else
		vos=store.listGroups();

	rapidjson::Document result(rapidjson::kObjectType);
	rapidjson::Document::AllocatorType& alloc = result.GetAllocator();
	
	result.AddMember("apiVersion", "v1alpha1", alloc);
	rapidjson::Value resultItems(rapidjson::kArrayType);
	resultItems.Reserve(vos.size(), alloc);
	for (const Group& group : vos){
		rapidjson::Value groupResult(rapidjson::kObjectType);
		groupResult.AddMember("name", group.name, alloc);
		groupResult.AddMember("display_name", group.displayName, alloc);
		groupResult.AddMember("email", group.email, alloc);
		groupResult.AddMember("phone", group.phone, alloc);
		groupResult.AddMember("purpose", group.purpose, alloc);
		groupResult.AddMember("description", group.description, alloc);
		resultItems.PushBack(groupResult, alloc);
	}
	result.AddMember("groups", resultItems, alloc);
	
	return crow::response(to_string(result));
}

crow::response createGroup(PersistentStore& store, const crow::request& req, 
                           std::string parentGroupName, std::string newGroupName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to create group " << newGroupName << " within " << parentGroupName);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	parentGroupName=canonicalizeGroupName(parentGroupName);
	newGroupName=canonicalizeGroupName(newGroupName,parentGroupName);
		
	Group parentGroup=store.getGroup(parentGroupName);
	if(!parentGroup) //the parent group must exist
		return crow::response(404,generateError("Parent group not found"));
	//only an admin in the parent group may create child groups
	//other members may request the creation of child groups
	if(!user.superuser && !store.userStatusInGroup(user.unixName,parentGroup.name).isMember())
		return crow::response(403,generateError("Not authorized"));
	{
		Group existingGroup=store.getGroup(newGroupName);
		if(existingGroup) //the group must not already exist
			return crow::response(400,generateError("Group already exists"));
	}
	
	//unpack the target info
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
		return crow::response(400,generateError("Incorrect type for configuration"));
	
	if(!body["metadata"].HasMember("name"))
		return crow::response(400,generateError("Missing Group name in request"));
	if(!body["metadata"]["name"].IsString())
		return crow::response(400,generateError("Incorrect type for Group name"));
	if(canonicalizeGroupName(body["metadata"]["name"].GetString(),parentGroupName)!=newGroupName)
		return crow::response(400,generateError("Group name in request does not match target URL path"));
	
	if(body["metadata"].HasMember("display_name") && !body["metadata"]["display_name"].IsString())
		return crow::response(400,generateError("Incorrect type for Group display name"));
	
	if(body["metadata"].HasMember("email") && !body["metadata"]["email"].IsString())
		return crow::response(400,generateError("Incorrect type for Group email"));
	if(body["metadata"].HasMember("phone") && !body["metadata"]["phone"].IsString())
		return crow::response(400,generateError("Incorrect type for Group phone"));
	
	if(!body["metadata"].HasMember("purpose"))
		return crow::response(400,generateError("Missing Group purpose in request"));
	if(!body["metadata"]["purpose"].IsString())
		return crow::response(400,generateError("Incorrect type for Group purpose"));
		
	if(body["metadata"].HasMember("description") && !body["metadata"]["description"].IsString())
		return crow::response(400,generateError("Incorrect type for Group description"));
		
	if(body["metadata"].HasMember("additional_attributes") && !body["metadata"]["additional_attributes"].IsObject())
		return crow::response(400,generateError("Incorrect type for Group additional attributes"));
	
	Group group;
	std::map<std::string,std::string> extraAttributes;
	
	//TODO: update name validation
	group.name=newGroupName;
	if(group.name.empty())
		return crow::response(400,generateError("Group names may not be the empty string"));
	
	if(body["metadata"].HasMember("display_name")){
		log_info("Getting display name from request");
		group.displayName=body["metadata"]["display_name"].GetString();
		if(group.displayName.empty()){
			log_info("requested name is empty, replacing with last component of FQGN");
			group.displayName=lastGroupComponent(group.name);
		}
	}
	else{
		log_info("no name requested, using last component of FQGN");
		group.displayName=lastGroupComponent(group.name);
	}
	log_info("Group display name will be " << group.displayName);
	
	if(body["metadata"].HasMember("email"))
		group.email=body["metadata"]["email"].GetString();
	else
		group.email=user.email;
	if(group.email.empty())
		group.email=" "; //Dynamo will get upset if a string is empty
		
	if(body["metadata"].HasMember("phone"))
		group.phone=body["metadata"]["phone"].GetString();
	else
		group.phone=user.phone;
	if(group.phone.empty())
		group.phone=" "; //Dynamo will get upset if a string is empty
	
	if(body["metadata"].HasMember("purpose"))
		group.purpose=normalizeScienceField(body["metadata"]["purpose"].GetString());
	if(group.purpose.empty())
		return crow::response(400,generateError("Unrecognized value for Group purpose\n"
		  "See http://slateci.io/docs/science-fields for a list of accepted values"));
	
	if(body["metadata"].HasMember("description"))
		group.description=body["metadata"]["description"].GetString();
	if(group.description.empty())
		group.description=" "; //Dynamo will get upset if a string is empty
	
	if(body["metadata"].HasMember("additional_attributes")){
		for(const auto& entry : body["metadata"].GetObject()){
			if(!entry.value.IsString())
				return crow::response(400,generateError("Incorrect type for Group additional attribute value"));
			extraAttributes[entry.name.GetString()]=entry.value.GetString();
		}
	}
	
	group.creationDate=timestamp();
	
	//if the user is a superuser or group admin, we just go ahead with creating the group
	if(user.superuser || store.userStatusInGroup(user.unixName,parentGroup.name).state==GroupMembership::Admin){
		group.valid=true;
	
		log_info("Creating Group " << group);
		bool created=store.addGroup(group);
		if(!created)
			return crow::response(500,generateError("Group creation failed"));
	
		//Make the creating user an initial member of the group
		GroupMembership initialAdmin;
		initialAdmin.userName=user.unixName;
		initialAdmin.groupName=group.name;
		initialAdmin.state=GroupMembership::Admin;
		initialAdmin.stateSetBy="user:"+user.unixName;
		initialAdmin.valid=true;
		bool added=store.setUserStatusInGroup(initialAdmin);
		if(!added){
			//TODO: possible problem: If we get here, we may end up with a valid group
			//but with no members and not return its ID either
			auto problem="Failed to add creating user "+
						 boost::lexical_cast<std::string>(user)+" to new Group "+
						 boost::lexical_cast<std::string>(group);
			log_error(problem);
			return crow::response(500,generateError(problem));
		}
	
		log_info("Created " << group << " on behalf of " << user);
		
		//if there are any extra attributes, store those too
		for(const auto attr : extraAttributes){
			if(!store.setGroupSecondaryAttribute(group.name,attr.first,attr.second))
				log_error("Failed to store group secondary attribute " << attr.first << '=' << attr.second);
		}
	}
	else{ //otherwise, store a group creation request for later approval by an admin
		GroupRequest gr(group,user.unixName);
		gr.valid=true;
		gr.secondaryAttributes=extraAttributes;
		
		log_info("Storing Group Request for " << gr);
		bool created=store.addGroupRequest(gr);
		if(!created)
			return crow::response(500,generateError("Group Request creation failed"));
		
		log_info("Created " << gr << " on behalf of " << user);
	}
	
	return crow::response(200);
}

crow::response getGroupInfo(PersistentStore& store, const crow::request& req, std::string groupName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested information about " << groupName);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	//Any user in the system may query a Group's information
	
	groupName=canonicalizeGroupName(groupName);
	Group group = store.getGroup(groupName);
	
	if(!group)
		return crow::response(404,generateError("Group not found"));

	rapidjson::Document result(rapidjson::kObjectType);
	rapidjson::Document::AllocatorType& alloc = result.GetAllocator();
	
	result.AddMember("apiVersion", "v1alpha1", alloc);
	rapidjson::Value metadata(rapidjson::kObjectType);
	metadata.AddMember("name", group.name, alloc);
	metadata.AddMember("display_name", group.displayName, alloc);
	metadata.AddMember("email", group.email, alloc);
	metadata.AddMember("phone", group.phone, alloc);
	metadata.AddMember("purpose", group.purpose, alloc);
	metadata.AddMember("description", group.description, alloc);
	result.AddMember("kind", "Group", alloc);
	result.AddMember("metadata", metadata, alloc);
	
	return crow::response(to_string(result));
}

crow::response updateGroup(PersistentStore& store, const crow::request& req, std::string groupName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to update " << groupName);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
		
	groupName=canonicalizeGroupName(groupName);
	//Only superusers and admins of a Group can alter it
	if(!user.superuser && store.userStatusInGroup(user.unixName,groupName).state!=GroupMembership::Admin)
		return crow::response(403,generateError("Not authorized"));
	
	Group targetGroup = store.getGroup(groupName);
	
	if(!targetGroup)
		return crow::response(404,generateError("Group not found"));
	
	//unpack the new Group info
	rapidjson::Document body;
	try{
		body.Parse(req.body.c_str());
	}catch(std::runtime_error& err){
		return crow::response(400,generateError("Invalid JSON in request body"));
	}
	if(body.IsNull())
		return crow::response(400,generateError("Invalid JSON in request body"));
	if(!body.HasMember("metadata"))
		return crow::response(400,generateError("Missing Group metadata in request"));
	if(!body["metadata"].IsObject())
		return crow::response(400,generateError("Incorrect type for metadata"));
		
	bool doUpdate=false;
	if(body["metadata"].HasMember("display_name")){
		if(!body["metadata"]["display_name"].IsString())
			return crow::response(400,generateError("Incorrect type for display name"));	
		targetGroup.displayName=body["metadata"]["display_name"].GetString();
		doUpdate=true;
	}
	if(body["metadata"].HasMember("email")){
		if(!body["metadata"]["email"].IsString())
			return crow::response(400,generateError("Incorrect type for email"));	
		targetGroup.email=body["metadata"]["email"].GetString();
		doUpdate=true;
	}
	if(body["metadata"].HasMember("phone")){
		if(!body["metadata"]["phone"].IsString())
			return crow::response(400,generateError("Incorrect type for phone"));	
		targetGroup.phone=body["metadata"]["phone"].GetString();
		doUpdate=true;
	}
	if(body["metadata"].HasMember("purpose")){
		if(!body["metadata"]["purpose"].IsString())
			return crow::response(400,generateError("Incorrect type for purpose"));	
		targetGroup.purpose=normalizeScienceField(body["metadata"]["purpose"].GetString());
		if(targetGroup.purpose.empty())
			return crow::response(400,generateError("Unrecognized value for Group purpose"));
		doUpdate=true;
	}
	if(body["metadata"].HasMember("description")){
		if(!body["metadata"]["description"].IsString())
			return crow::response(400,generateError("Incorrect type for description"));	
		targetGroup.description=body["metadata"]["description"].GetString();
		doUpdate=true;
	}
	
	if(!doUpdate){
		log_info("Requested update to " << targetGroup << " is trivial");
		return(crow::response(200));
	}
	
	log_info("Updating " << targetGroup);
	bool success=store.updateGroup(targetGroup);
	
	if(!success){
		log_error("Failed to update " << targetGroup);
		return crow::response(500,generateError("Group update failed"));
	}
	
	return(crow::response(200));
}

crow::response deleteGroup(PersistentStore& store, const crow::request& req, std::string groupName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to delete " << groupName);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	groupName=canonicalizeGroupName(groupName);
	//Only superusers and admins of a Group can alter it
	if(!user.superuser && store.userStatusInGroup(user.unixName,groupName).state!=GroupMembership::Admin)
		return crow::response(403,generateError("Not authorized"));
	
	Group targetGroup = store.getGroup(groupName);
	
	if(!targetGroup)
		return crow::response(404,generateError("Group not found"));
	
	log_info("Deleting " << targetGroup);
	bool deleted = store.removeGroup(targetGroup.name);

	if (!deleted)
		return crow::response(500, generateError("Group deletion failed"));
	
	return(crow::response(200));
}

crow::response listGroupMembers(PersistentStore& store, const crow::request& req, std::string groupName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to list members of " << groupName);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	
	groupName=canonicalizeGroupName(groupName);
	Group targetGroup = store.getGroup(groupName);
	if(!targetGroup)
		return crow::response(404,generateError("Group not found"));
	
	auto memberships=store.getMembersOfGroup(targetGroup.name);
	
	rapidjson::Document result(rapidjson::kObjectType);
	rapidjson::Document::AllocatorType& alloc = result.GetAllocator();
	
	result.AddMember("apiVersion", "v1alpha1", alloc);
	rapidjson::Value resultItems(rapidjson::kArrayType);
	resultItems.Reserve(memberships.size(), alloc);
	for(const auto& membership : memberships){
		rapidjson::Value userResult(rapidjson::kObjectType);
		userResult.AddMember("user_name", membership.userName, alloc);
		userResult.AddMember("state", GroupMembership::to_string(membership.state), alloc);
		userResult.AddMember("state_set_by", membership.stateSetBy, alloc);
		resultItems.PushBack(userResult, alloc);
	}
	result.AddMember("memberships", resultItems, alloc);
	
	return crow::response(to_string(result));
}

crow::response getGroupMemberStatus(PersistentStore& store, const crow::request& req, const std::string& userID, std::string groupName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to get membership status of " << userID << " in " << groupName);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	
	groupName=canonicalizeGroupName(groupName);
	GroupMembership membership=store.userStatusInGroup(userID, groupName);
	
	rapidjson::Document result(rapidjson::kObjectType);
	rapidjson::Document::AllocatorType& alloc = result.GetAllocator();
	
	result.AddMember("apiVersion", "v1alpha1", alloc);
	rapidjson::Value data(rapidjson::kObjectType);
	data.AddMember("user_name", membership.userName, alloc);
	data.AddMember("state", GroupMembership::to_string(membership.state), alloc);
	data.AddMember("state_set_by", membership.stateSetBy, alloc);		
	result.AddMember("membership", data, alloc);
	
	return crow::response(to_string(result));
}

crow::response getSubgroups(PersistentStore& store, const crow::request& req, std::string groupName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to get subgroups of " << groupName);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	
	groupName=canonicalizeGroupName(groupName);
	Group parentGroup = store.getGroup(groupName);
	if(!parentGroup)
		return crow::response(404,generateError("Group not found"));
	
	std::string filterPrefix=groupName+".";
	std::vector<Group> allGroups=store.listGroups();

	rapidjson::Document result(rapidjson::kObjectType);
	rapidjson::Document::AllocatorType& alloc = result.GetAllocator();
	
	result.AddMember("apiVersion", "v1alpha1", alloc);
	rapidjson::Value resultItems(rapidjson::kArrayType);
	for (const Group& group : allGroups){
		if(group.name.find(filterPrefix)!=0)
			continue;
		rapidjson::Value groupResult(rapidjson::kObjectType);
		groupResult.AddMember("name", group.name, alloc);
		groupResult.AddMember("display_name", group.displayName, alloc);
		groupResult.AddMember("email", group.email, alloc);
		groupResult.AddMember("phone", group.phone, alloc);
		groupResult.AddMember("purpose", group.purpose, alloc);
		groupResult.AddMember("description", group.description, alloc);
		resultItems.PushBack(groupResult, alloc);
	}
	result.AddMember("groups", resultItems, alloc);
	
	return crow::response(to_string(result));
}

crow::response getSubgroupRequests(PersistentStore& store, const crow::request& req, std::string groupName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to get subgroups of " << groupName);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	
	groupName=canonicalizeGroupName(groupName);
	Group parentGroup = store.getGroup(groupName);
	if(!parentGroup)
		return crow::response(404,generateError("Group not found"));
	
	std::string filterPrefix=groupName+".";
	std::vector<GroupRequest> allGroups=store.listGroupRequests();

	rapidjson::Document result(rapidjson::kObjectType);
	rapidjson::Document::AllocatorType& alloc = result.GetAllocator();
	
	result.AddMember("apiVersion", "v1alpha1", alloc);
	rapidjson::Value resultItems(rapidjson::kArrayType);
	for (const GroupRequest& group : allGroups){
		if(group.name.find(filterPrefix)!=0)
			continue;
		rapidjson::Value groupResult(rapidjson::kObjectType);
		groupResult.AddMember("name", group.name, alloc);
		groupResult.AddMember("display_name", group.displayName, alloc);
		groupResult.AddMember("email", group.email, alloc);
		groupResult.AddMember("phone", group.phone, alloc);
		groupResult.AddMember("purpose", group.purpose, alloc);
		groupResult.AddMember("description", group.description, alloc);
		groupResult.AddMember("requester", group.requester, alloc);
		resultItems.PushBack(groupResult, alloc);
	}
	result.AddMember("groups", resultItems, alloc);
	
	return crow::response(to_string(result));
}

crow::response approveSubgroupRequest(PersistentStore& store, const crow::request& req, std::string parentGroupName, std::string newGroupName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to approve creation of the " << newGroupName << " subgroup of " << parentGroupName);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
		
	parentGroupName=canonicalizeGroupName(parentGroupName);
	//Only superusers and admins of a Group can alter it
	if(!user.superuser && store.userStatusInGroup(user.unixName,parentGroupName).state!=GroupMembership::Admin)
		return crow::response(403,generateError("Not authorized"));
	
	newGroupName=canonicalizeGroupName(newGroupName,parentGroupName);
	Group newGroup = store.getGroup(newGroupName);
	if(!newGroup.pending)
		return crow::response(400,generateError("Group already exists"));
	
	GroupRequest newGroupRequest = store.getGroupRequest(newGroupName);
	
	log_info("Approving creation of subgroup " << newGroupName);
	bool success=store.approveGroupRequest(newGroupName);
	
	GroupMembership initialAdmin;
	initialAdmin.userName=newGroupRequest.requester;
	initialAdmin.groupName=newGroupRequest.name;
	initialAdmin.state=GroupMembership::Admin;
	initialAdmin.stateSetBy="user:"+user.unixName;
	initialAdmin.valid=true;
	bool added=store.setUserStatusInGroup(initialAdmin);
	if(!added){
		//TODO: possible problem: If we get here, we may end up with a valid group
		//but with no members
		auto problem="Failed to add creating user "+
					 boost::lexical_cast<std::string>(user)+" to new Group "+
					 boost::lexical_cast<std::string>(newGroupRequest);
		log_error(problem);
		return crow::response(500,generateError(problem));
	}
	
	if (!success)
		return crow::response(500, generateError("Storing group request approval failed"));
	
	return(crow::response(200));
}

crow::response denySubgroupRequest(PersistentStore& store, const crow::request& req, std::string parentGroupName, std::string newGroupName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to deny creation of the " << newGroupName << " subgroup of " << parentGroupName);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
		
	parentGroupName=canonicalizeGroupName(parentGroupName);
	//Only superusers and admins of a Group can alter it
	if(!user.superuser && store.userStatusInGroup(user.unixName,parentGroupName).state!=GroupMembership::Admin)
		return crow::response(403,generateError("Not authorized"));
	
	newGroupName=canonicalizeGroupName(newGroupName);
	Group newGroup = store.getGroup(newGroupName);
	if(!newGroup.pending)
		return crow::response(400,generateError("Group already exists"));
	
	bool success = store.removeGroup(newGroupName);
	
	if (!success)
		return crow::response(500, generateError("Deleting group request failed"));
	
	return(crow::response(200));
}

crow::response getGroupAttribute(PersistentStore& store, const crow::request& req, std::string groupName, std::string attributeName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to fetch secondary attribute " << attributeName << " of group " << groupName);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	
	groupName=canonicalizeGroupName(groupName);
	//Any user can query any group property?
	
	std::string value=store.getGroupSecondaryAttribute(groupName, attributeName);
	if(value.empty())
		return crow::response(404,generateError("Group or attribute not found"));
	
	rapidjson::Document result(rapidjson::kObjectType);
	rapidjson::Document::AllocatorType& alloc = result.GetAllocator();
	
	result.AddMember("apiVersion", "v1alpha1", alloc);
	result.AddMember("data", value, alloc);
	
	return crow::response(to_string(result));
}

crow::response setGroupAttribute(PersistentStore& store, const crow::request& req, std::string groupName, std::string attributeName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to set secondary attribute " << attributeName << " for group " << groupName);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
		
	groupName=canonicalizeGroupName(groupName);
	//Only superusers and admins of a Group can alter it
	if(!user.superuser && store.userStatusInGroup(user.unixName,groupName).state!=GroupMembership::Admin)
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
	bool success=store.setGroupSecondaryAttribute(groupName, attributeName, attributeValue);
	
	if(!success)
		return crow::response(500,generateError("Failed to store group attribute"));
	
	return crow::response(200);
}

crow::response deleteGroupAttribute(PersistentStore& store, const crow::request& req, std::string groupName, std::string attributeName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to delete secondary attribute " << attributeName << " from group " << groupName);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	
	groupName=canonicalizeGroupName(groupName);
	//Only superusers and admins of a Group can alter it
	if(!user.superuser && store.userStatusInGroup(user.unixName,groupName).state!=GroupMembership::Admin)
		return crow::response(403,generateError("Not authorized"));
	
	bool success=store.removeGroupSecondaryAttribute(groupName, attributeName);
	
	if(!success)
		return crow::response(500,generateError("Failed to delete group attribute"));
	
	return crow::response(200);
}

crow::response getScienceFields(PersistentStore& store, const crow::request& req){
	rapidjson::Document result(rapidjson::kObjectType);
	rapidjson::Document::AllocatorType& alloc = result.GetAllocator();
	
	result.AddMember("apiVersion", "v1alpha1", alloc);
	rapidjson::Value resultItems(rapidjson::kArrayType);
	for (const auto& field : scienceFields){
		rapidjson::Value item(rapidjson::kStringType);
		item.SetString(field, alloc);
		resultItems.PushBack(item, alloc);
	}
	result.AddMember("fields_of_science", resultItems, alloc);
	
	return crow::response(to_string(result));
}