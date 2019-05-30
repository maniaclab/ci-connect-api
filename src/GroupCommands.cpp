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
	
	result.AddMember("apiVersion", "v1alpha3", alloc);
	rapidjson::Value resultItems(rapidjson::kArrayType);
	resultItems.Reserve(vos.size(), alloc);
	for (const Group& group : vos){
		rapidjson::Value groupResult(rapidjson::kObjectType);
		groupResult.AddMember("name", group.name, alloc);
		groupResult.AddMember("email", group.email, alloc);
		groupResult.AddMember("phone", group.phone, alloc);
		groupResult.AddMember("field_of_science", group.scienceField, alloc);
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
	if(store.userStatusInGroup(user.id,parentGroup.name).state!=GroupMembership::Admin)
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
	
	if(body["metadata"].HasMember("email") && !body["metadata"]["email"].IsString())
		return crow::response(400,generateError("Incorrect type for Group email"));
	if(body["metadata"].HasMember("phone") && !body["metadata"]["phone"].IsString())
		return crow::response(400,generateError("Incorrect type for Group phone"));
	
	if(!body["metadata"].HasMember("field_of_science"))
		return crow::response(400,generateError("Missing Group field of science in request"));
	if(!body["metadata"]["field_of_science"].IsString())
		return crow::response(400,generateError("Incorrect type for Group field of science"));
		
	if(body["metadata"].HasMember("description") && !body["metadata"]["description"].IsString())
		return crow::response(400,generateError("Incorrect type for Group description"));
	
	Group group;
	
	//TODO: update name validation
	group.name=newGroupName;
	if(group.name.empty())
		return crow::response(400,generateError("Group names may not be the empty string"));
	
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
	
	if(body["metadata"].HasMember("field_of_science"))
		group.scienceField=normalizeScienceField(body["metadata"]["field_of_science"].GetString());
	if(group.scienceField.empty())
		return crow::response(400,generateError("Unrecognized value for Group field of science\n"
		  "See http://slateci.io/docs/science-fields for a list of accepted values"));
	
	if(body["metadata"].HasMember("description"))
		group.description=body["metadata"]["description"].GetString();
	if(group.description.empty())
		group.description=" "; //Dynamo will get upset if a string is empty
	
	group.valid=true;
	
	log_info("Creating Group " << group);
	bool created=store.addGroup(group);
	if(!created)
		return crow::response(500,generateError("Group creation failed"));
	
	//Make the creating user an initial member of the group
	GroupMembership initialAdmin;
	initialAdmin.userID=user.id;
	initialAdmin.groupName=group.name;
	initialAdmin.state=GroupMembership::Admin;
	initialAdmin.stateSetBy=user.id;
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

	rapidjson::Document result(rapidjson::kObjectType);
	rapidjson::Document::AllocatorType& alloc = result.GetAllocator();
	
	result.AddMember("apiVersion", "v1alpha3", alloc);
	result.AddMember("kind", "Group", alloc);
	rapidjson::Value metadata(rapidjson::kObjectType);
	metadata.AddMember("name", rapidjson::StringRef(group.name.c_str()), alloc);
	metadata.AddMember("email", rapidjson::StringRef(group.email.c_str()), alloc);
	metadata.AddMember("phone", rapidjson::StringRef(group.phone.c_str()), alloc);
	metadata.AddMember("field_of_science", rapidjson::StringRef(group.scienceField.c_str()), alloc);
	metadata.AddMember("description", rapidjson::StringRef(group.description.c_str()), alloc);
	result.AddMember("metadata", metadata, alloc);
	
	return crow::response(to_string(result));
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
	
	result.AddMember("apiVersion", "v1alpha3", alloc);
	rapidjson::Value metadata(rapidjson::kObjectType);
	metadata.AddMember("name", rapidjson::StringRef(group.name.c_str()), alloc);
	metadata.AddMember("email", rapidjson::StringRef(group.email.c_str()), alloc);
	metadata.AddMember("phone", rapidjson::StringRef(group.phone.c_str()), alloc);
	metadata.AddMember("field_of_science", rapidjson::StringRef(group.scienceField.c_str()), alloc);
	metadata.AddMember("description", rapidjson::StringRef(group.description.c_str()), alloc);
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
	if(!user.superuser || store.userStatusInGroup(user.id,groupName).state!=GroupMembership::Admin)
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
	if(body["metadata"].HasMember("field_of_science")){
		if(!body["metadata"]["field_of_science"].IsString())
			return crow::response(400,generateError("Incorrect type for field of science"));	
		targetGroup.scienceField=normalizeScienceField(body["metadata"]["field_of_science"].GetString());
		if(targetGroup.scienceField.empty())
			return crow::response(400,generateError("Unrecognized value for Group field of science"));
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
	if(!user.superuser || store.userStatusInGroup(user.id,groupName).state!=GroupMembership::Admin)
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
	
	result.AddMember("apiVersion", "v1alpha3", alloc);
	rapidjson::Value resultItems(rapidjson::kArrayType);
	resultItems.Reserve(memberships.size(), alloc);
	for(const auto& membership : memberships){
		rapidjson::Value userResult(rapidjson::kObjectType);
		userResult.AddMember("id", membership.userID, alloc);
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
	
	result.AddMember("apiVersion", "v1alpha3", alloc);
	rapidjson::Value data(rapidjson::kObjectType);
	data.AddMember("user_id", membership.userID, alloc);
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
	
	result.AddMember("apiVersion", "v1alpha3", alloc);
	rapidjson::Value resultItems(rapidjson::kArrayType);
	for (const Group& group : allGroups){
		if(group.name.find(filterPrefix)!=0)
			continue;
		rapidjson::Value groupResult(rapidjson::kObjectType);
		groupResult.AddMember("name", group.name, alloc);
		groupResult.AddMember("email", group.email, alloc);
		groupResult.AddMember("phone", group.phone, alloc);
		groupResult.AddMember("field_of_science", group.scienceField, alloc);
		groupResult.AddMember("description", group.description, alloc);
		resultItems.PushBack(groupResult, alloc);
	}
	result.AddMember("groups", resultItems, alloc);
	
	return crow::response(to_string(result));
}
