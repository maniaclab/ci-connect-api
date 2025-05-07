#include "GroupCommands.h"

#include <boost/lexical_cast.hpp>

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

#include "Logging.h"
#include "ServerUtilities.h"
#include "server_version.h"
#include "UserCommands.h"

namespace{
	//Science categories defined in https://www.nsf.gov/statistics/nsf13327/pdf/tabb1.pdf
	const char* scienceFields[]={
		"Advanced Scientific Computing",
		"Agronomy",
		"Applied Mathematics",
		"Astronomy",
		"Astronomy and Astrophysics",
		"Astronomical Sciences",
		"Astrophysics",
		"Atmospheric Sciences",
		"Biochemistry",
		"Bioinformatics",
		"Biological Sciences",
		"Biological and Biomedical Sciences",
		"Biological and Critical Systems",
		"Biomedical research",
		"Biophysics",
		"Biostatistics",
		"Cellular Biology",
		"Chemical Engineering",
		"Chemical Sciences",
		"Chemistry",
		"Civil Engineering",
		"Community Grid",
		"Complex Adaptive Systems",
		"Computational Biology",
		"Computational Condensed Matter Physics",
		"Computer Science",
		"Computer and Information Services",
		"Computer and Information Science and Engineering",
		"Condensed Matter Physics",
		"Earth Sciences",
		"Ecological and Environmental Sciences",
		"Economics",
		"Education",
		"Educational Psychology",
		"Elementary Particles",
		"Engineering",
		"Evolutionary Biology",
		"Evolutionary Sciences",
		"Finance",
		"Fluid Dynamics",
		"Genetics and Nucleic Acids",
		"Genomics",
		"Geographic Information Science",
		"Geography",
		"Geological and Earth Sciences",
		"Gravitational Physics",
		"High Energy Physics",
		"Information Theory",
		"Information, Robotics, and Intelligent Systems",
		"Infrastructure Development",
		"Logic",
		"Materials Research",
		"Materials Science",
		"Mathematical Sciences",
		"Mathematics",
		"Medical Imaging",
		"Medical Sciences",
		"Microbiology",
		"Molecular and Structural Biosciences",
		"Multi-Science Community",
		"Multidisciplinary",
		"Nanoelectronics",
		"National Laboratory",
		"Network Science",
		"Neuroscience",
		"Nuclear Physics",
		"Nutritional Science",
		"Ocean Sciences",
		"Other",
		"Particle Physics",
		"Physical Chemistry",
		"Physical Therapy",
		"Physics",
		"Physics and astronomy",
		"Physiology",
		"Planetary Astronomy",
		"Plant Biology",
		"Research Computing",
		"Statistics",
		"Technology",
		"Training",
		"Zoology",
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
	log_info(user << " requested to list groups from " << req.remote_endpoint);
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
		groupResult.AddMember("creation_date", group.creationDate, alloc);
		groupResult.AddMember("unix_id", group.unixID, alloc);
		groupResult.AddMember("pending", group.pending, alloc);
		resultItems.PushBack(groupResult, alloc);
	}
	result.AddMember("groups", resultItems, alloc);
	
	return crow::response(to_string(result));
}

crow::response createGroup(PersistentStore& store, const crow::request& req, 
                           std::string parentGroupName, std::string newGroupName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to create group " << newGroupName << " within " << parentGroupName << " from " << req.remote_endpoint);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	parentGroupName=canonicalizeGroupName(parentGroupName);
	newGroupName=canonicalizeGroupName(newGroupName,parentGroupName);
		
	Group parentGroup=store.getGroup(parentGroupName);
	if(!parentGroup) //the parent group must exist
		return crow::response(404,generateError("Parent group not found"));
	//only an admin in the parent group may create child groups
	//other members may request the creation of child groups
	if(!user.superuser && !store.userStatusInGroup(user.unixName,parentGroup.name).isMember()
	   && adminInAnyEnclosingGroup(store,user.unixName,parentGroup.name).empty())
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
		
	if(body["metadata"].HasMember("unix_id") && !body["metadata"]["unix_id"].IsUint()){
		log_warn("Unix ID in group creation request was not an unsigned integer");
		return crow::response(400,generateError("Incorrect type for group unix ID"));
	}
		
	if(body["metadata"].HasMember("additional_attributes") && !body["metadata"]["additional_attributes"].IsObject())
		return crow::response(400,generateError("Incorrect type for Group additional attributes"));
	
	Group group;
	std::map<std::string,std::string> extraAttributes;
	
	//TODO: update name validation
	group.name=newGroupName;
	//Group names must conform to /[a-zA-Z0-9_][a-zA-Z0-9_-]*/
	std::string unqualifiedGroupName=lastGroupComponent(group.name);
	if(unqualifiedGroupName.empty())
		return crow::response(400,generateError("Group names may not be the empty string"));
	const static std::string groupNameLeadCharacters=
		"abcdefghijklmnopqrstuvwxyz"
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		"0123456789_";
	const static std::string groupNameFollowingCharacters=groupNameLeadCharacters+"-";
	//take advantage of the following character set being a superset of the lead character set
	if(groupNameLeadCharacters.find(unqualifiedGroupName[0])==std::string::npos
	  || unqualifiedGroupName.find_first_not_of(groupNameFollowingCharacters)!=std::string::npos)
		return crow::response(400,generateError("Group names must match the regular expression [a-zA-Z0-9_][a-zA-Z0-9_-]*"));
	
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
		group.purpose=body["metadata"]["purpose"].GetString();//normalizeScienceField(body["metadata"]["purpose"].GetString());
	if(group.purpose.empty())
		return crow::response(400,generateError("Unrecognized value for Group purpose\n"
		  "See http://slateci.io/docs/science-fields for a list of accepted values"));
	
	if(body["metadata"].HasMember("description"))
		group.description=body["metadata"]["description"].GetString();
	if(group.description.empty())
		group.description=" "; //Dynamo will get upset if a string is empty
	
	if(body["metadata"].HasMember("unix_id")){
		group.unixID=body["metadata"]["unix_id"].GetUint();
	}
	
	if(body["metadata"].HasMember("additional_attributes")){
		for(const auto& entry : body["metadata"]["additional_attributes"].GetObject()){
			if(!entry.value.IsString())
				return crow::response(400,generateError("Incorrect type for Group additional attribute value"));
			std::string key=entry.name.GetString();
			std::string value=entry.value.GetString();
			if(key.empty() || value.empty())
				return crow::response(400,generateError("Additional group attribute keys and values cannot be empty strings"));
			extraAttributes[key]=value;
		}
	}
	
	group.creationDate=timestamp();
	
	//if the user is a superuser, group admin, or admin of an enclosing group, 
	//we just go ahead with creating the group
	if(user.superuser || 
	   store.userStatusInGroup(user.unixName,parentGroup.name).state==GroupMembership::Admin
	   || !adminInAnyEnclosingGroup(store,user.unixName,parentGroup.name).empty()){
		group.valid=true;
	
		log_info("Creating Group " << group);
		bool created=store.addGroup(group);
		if(!created)
			return crow::response(500,generateError("Group creation failed"));
	
		if(store.userStatusInGroup(user.unixName,parentGroup.name).state!=GroupMembership::NonMember){
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
		
		//inform people of the request
		EmailClient::Email adminMessage;
		adminMessage.fromAddress="noreply@api.ci-connect.net";
		adminMessage.toAddresses={parentGroup.email};
		for(const auto& membership : store.getMembersOfGroup(parentGroup.name)){
			if(membership.state==GroupMembership::Admin){
				User admin=store.getUser(membership.userName);
				adminMessage.toAddresses.push_back(admin.email);
			}
		}
		adminMessage.replyTo=user.email;
		adminMessage.subject="CI-Connect group creation request";
		adminMessage.body="This is an automatic notification that "+user.name+
		" ("+user.unixName+") has requested to create a subgroup, "+gr.displayName+
		" ("+gr.name+") within the "+parentGroup.displayName+" group.";
		store.getEmailClient().sendEmail(adminMessage);
		
		EmailClient::Email userMessage;
		userMessage.subject=adminMessage.subject;
		userMessage.fromAddress="noreply@api.ci-connect.net";
		userMessage.toAddresses={user.email};
		userMessage.replyTo=group.email;
		userMessage.body="This is an automatic notification that your request to create a subgroup "
						 +gr.displayName+" ("+gr.name+") within the "+parentGroup.displayName
						 +" group is being processed.";
		store.getEmailClient().sendEmail(userMessage);
	}
	
	return crow::response(200);
}

crow::response getGroupInfo(PersistentStore& store, const crow::request& req, std::string groupName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested information about " << groupName << " from " << req.remote_endpoint);
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
	metadata.AddMember("creation_date", group.creationDate, alloc);
	metadata.AddMember("unix_id", group.unixID, alloc);
	metadata.AddMember("pending", group.pending, alloc);
	result.AddMember("kind", "Group", alloc);
	result.AddMember("metadata", metadata, alloc);
	
	return crow::response(to_string(result));
}

crow::response updateGroup(PersistentStore& store, const crow::request& req, std::string groupName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to update " << groupName << " from " << req.remote_endpoint);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	
	Group targetGroup = store.getGroup(groupName);
	if(targetGroup.pending){
		log_info("Target group is in a pending state, treating as a group request update");
		return updateGroupRequest(store, req, groupName);
	}
	
	if(!targetGroup)
		return crow::response(404,generateError("Group not found"));
		
	groupName=canonicalizeGroupName(groupName);
	//Only superusers and admins of a Group can alter it
	if(!user.superuser && 
	   store.userStatusInGroup(user.unixName,groupName).state!=GroupMembership::Admin &&
	   adminInAnyEnclosingGroup(store,user.unixName,groupName).empty())
		return crow::response(403,generateError("Not authorized"));
	
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

crow::response updateGroupRequest(PersistentStore& store, const crow::request& req, std::string groupName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to update information for " << groupName << " from " << req.remote_endpoint);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	
	groupName=canonicalizeGroupName(groupName);
	
	GroupRequest targetRequest = store.getGroupRequest(groupName);
	if(!targetRequest)
		return crow::response(404,generateError("Group request not found"));
	
	//There are no members of a group request; the relevant authorities are the 
	//enclosing group admins and the requester. 
	std::string enclosingGroupName=enclosingGroup(groupName);
	if(!user.superuser && 
	  store.userStatusInGroup(user.unixName,enclosingGroupName).state!=GroupMembership::Admin
	  && adminInAnyEnclosingGroup(store,user.unixName,enclosingGroupName).empty()
	  && user.unixName!=targetRequest.requester)
		return crow::response(403,generateError("Not authorized"));
	
	//unpack the new info
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
	bool nameChange=false;
	
	if(body["metadata"].HasMember("name")){
		if(!body["metadata"]["name"].IsString())
			return crow::response(400,generateError("Incorrect type for name"));
		//Changing the requested group name is a bit tricky. 
		//We need to make sure that it is fully qualified and doesn't collide with anything else
		std::string requestedName=body["metadata"]["name"].GetString();
		//first ensure that the name is fully qualified
		requestedName=canonicalizeGroupName(requestedName, enclosingGroupName);
		//then check for collisions
		Group otherGroup=store.getGroup(requestedName);
		if(otherGroup.valid)
			return crow::response(400,generateError("A group named "+requestedName+" already exists"));
		//TODO: it would be problematic if the requested name attempts to move 
		//the request somewhere else in the group hierarchy
		targetRequest.name=requestedName;
		doUpdate=true;
		nameChange=true;
	}
	if(body["metadata"].HasMember("display_name")){
		if(!body["metadata"]["display_name"].IsString())
			return crow::response(400,generateError("Incorrect type for display name"));
		targetRequest.displayName=body["metadata"]["display_name"].GetString();
		doUpdate=true;
	}
	if(body["metadata"].HasMember("email")){
		if(!body["metadata"]["email"].IsString())
			return crow::response(400,generateError("Incorrect type for email"));
		targetRequest.email=body["metadata"]["email"].GetString();
		doUpdate=true;
	}
	if(body["metadata"].HasMember("phone")){
		if(!body["metadata"]["phone"].IsString())
			return crow::response(400,generateError("Incorrect type for phone"));
		targetRequest.phone=body["metadata"]["phone"].GetString();
		doUpdate=true;
	}
	if(body["metadata"].HasMember("purpose")){
		if(!body["metadata"]["purpose"].IsString())
			return crow::response(400,generateError("Incorrect type for purpose"));
		targetRequest.purpose=normalizeScienceField(body["metadata"]["purpose"].GetString());
		if(targetRequest.purpose.empty())
			return crow::response(400,generateError("Unrecognized value for Group purpose"));
		doUpdate=true;
	}
	if(body["metadata"].HasMember("description")){
		if(!body["metadata"]["description"].IsString())
			return crow::response(400,generateError("Incorrect type for description"));
		targetRequest.description=body["metadata"]["description"].GetString();
		doUpdate=true;
	}
	if(body["metadata"].HasMember("additional_attributes")){
		if(!body["metadata"]["additional_attributes"].IsObject())
			return crow::response(400,generateError("Incorrect type for additional attributes"));
		for(const auto& entry : body["metadata"]["additional_attributes"].GetObject()){
			if(!entry.value.IsString())
				return crow::response(400,generateError("Incorrect type for Group additional attribute value"));
			std::string key=entry.name.GetString();
			std::string value=entry.value.GetString();
			if(key.empty() || value.empty())
				return crow::response(400,generateError("Additional group attribute keys and values cannot be empty strings"));
			targetRequest.secondaryAttributes[key]=value;
			doUpdate=true;
		}
	}
	
	
	if(!doUpdate){
		log_info("Requested update to " << targetRequest << " is trivial");
		return(crow::response(200));
	}
	
	log_info("Updating " << targetRequest);
	bool success=false;
	if(nameChange){
		success=store.addGroupRequest(targetRequest); //first create new record
		if(!success){
			log_error("Failed to create " << targetRequest << " under new name");
			return crow::response(500,generateError("Group request update failed"));
		}
		success=store.removeGroup(groupName); //then remove the old record
		if(!success){
			log_error("Failed to remove old " << targetRequest << " record ("+groupName+")");
			return crow::response(500,generateError("Group request update failed"));
		}
	}
	else
		success=store.updateGroupRequest(targetRequest);
	
	if(!success){
		log_error("Failed to update " << targetRequest);
		return crow::response(500,generateError("Group request update failed"));
	}
	
	return(crow::response(200));
}

crow::response deleteGroup(PersistentStore& store, const crow::request& req, std::string groupName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to delete " << groupName << " from " << req.remote_endpoint);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	groupName=canonicalizeGroupName(groupName);
	//Only superusers and admins of a Group can alter it
	if(!user.superuser && 
	   store.userStatusInGroup(user.unixName,groupName).state!=GroupMembership::Admin
	   && adminInAnyEnclosingGroup(store,user.unixName,groupName).empty())
		return crow::response(403,generateError("Not authorized"));
	
	Group targetGroup = store.getGroup(groupName);
	
	if(!targetGroup)
		return crow::response(404,generateError("Group not found"));
	
	//collect all members of the group before deleting it
	auto memberships=store.getMembersOfGroup(targetGroup.name);
	//figure out parent group
	const Group parentGroup=store.getGroup(enclosingGroup(groupName));
	
	log_info("Deleting " << targetGroup << " subgroups");
	std::string filterPrefix=groupName+".";
	//collect names of all subgroups
	std::vector<std::string> subgroups;
	for (const Group& group : store.listGroups()){
		if(group.name.find(filterPrefix)!=0)
			continue;
		subgroups.push_back(group.name);
	}
	//sort in reverse order so that we delete foo.bar.baz before foo.bar, etc.
	std::sort(subgroups.begin(),subgroups.end(),std::greater<std::string>{});
	for(auto group : subgroups){
		log_info("Deleting " << group);
		bool deleted = store.removeGroup(group);
		if (!deleted)
			return crow::response(500, generateError("Group deletion failed"));
	}
	
	log_info("Deleting " << targetGroup);
	bool deleted = store.removeGroup(targetGroup.name);
	if (!deleted)
		return crow::response(500, generateError("Group deletion failed"));
	
	//email parent group contact and all members of deleted group
	EmailClient::Email message;
	message.fromAddress="noreply@api.ci-connect.net";
	message.toAddresses={parentGroup.email};
	message.bccAddresses.reserve(memberships.size());
	for(const auto& membership : memberships){
		if(membership.state==GroupMembership::NonMember)
			continue; //ignore non-members who may have been reported
		message.bccAddresses.push_back(store.getUser(membership.userName).email);
	}
	message.subject="CI-Connect group deleted";
	message.body="This is an automatic notification that "+user.name+
	" ("+user.unixName+") has deleted the "+targetGroup.displayName+
	" ("+targetGroup.name+") group from the "+parentGroup.displayName+" group.";
	store.getEmailClient().sendEmail(message);
	
	return(crow::response(200));
}

crow::response listGroupMembers(PersistentStore& store, const crow::request& req, std::string groupName){
	using namespace std::chrono;
	high_resolution_clock::time_point t1 = high_resolution_clock::now();
	
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to list members of " << groupName << " from " << req.remote_endpoint);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	
	groupName=canonicalizeGroupName(groupName);
	Group targetGroup = store.getGroup(groupName);
	if(!targetGroup)
		return crow::response(404,generateError("Group not found"));
	
	try{
	auto memberships=store.getMembersOfGroup(targetGroup.name);
	log_info("Found " << memberships.size() << " members of " << groupName);
	
	rapidjson::Document result(rapidjson::kObjectType);
	rapidjson::Document::AllocatorType& alloc = result.GetAllocator();
	
	result.AddMember("apiVersion", "v1alpha1", alloc);
	rapidjson::Value resultItems(rapidjson::kArrayType);
	resultItems.Reserve(memberships.size(), alloc);
	for(const auto& membership : memberships){
		if(membership.state==GroupMembership::NonMember)
			continue;
		rapidjson::Value userResult(rapidjson::kObjectType);
		userResult.AddMember("user_name", membership.userName, alloc);
		userResult.AddMember("state", GroupMembership::to_string(membership.state), alloc);
		userResult.AddMember("state_set_by", membership.stateSetBy, alloc);
		resultItems.PushBack(userResult, alloc);
	}
	result.AddMember("memberships", resultItems, alloc);
	
	high_resolution_clock::time_point t2 = high_resolution_clock::now();
	log_info("Sending OK response with group membership data after " <<
	         duration_cast<duration<double>>(t2-t1).count() << " seconds");
	return crow::response(to_string(result));
	}catch(std::exception& ex){
		log_error("Failure providing group membership data: " << ex.what());
		throw;
	}catch(...){
		log_error("Unknown failure providing group membership data");
		throw;
	}
}

crow::response getGroupMemberStatus(PersistentStore& store, const crow::request& req, const std::string& userID, std::string groupName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to get membership status of " << userID << " in " << groupName << " from " << req.remote_endpoint);
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
	log_info(user << " requested to get subgroups of " << groupName << " from " << req.remote_endpoint);
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
		groupResult.AddMember("creation_date", group.creationDate, alloc);
		groupResult.AddMember("unix_id", group.unixID, alloc);
		groupResult.AddMember("pending", group.pending, alloc);
		resultItems.PushBack(groupResult, alloc);
	}
	result.AddMember("groups", resultItems, alloc);
	
	return crow::response(to_string(result));
}

crow::response getSubgroupRequests(PersistentStore& store, const crow::request& req, std::string groupName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to get subgroups of " << groupName << " from " << req.remote_endpoint);
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
		rapidjson::Value secondary(rapidjson::kObjectType);
		for(const auto& attr : group.secondaryAttributes)
			secondary.AddMember(rapidjson::Value(attr.first,alloc), rapidjson::Value(attr.second,alloc), alloc);
		groupResult.AddMember("additional_attributes", secondary, alloc);
		resultItems.PushBack(groupResult, alloc);
	}
	result.AddMember("groups", resultItems, alloc);
	
	return crow::response(to_string(result));
}

crow::response approveSubgroupRequest(PersistentStore& store, const crow::request& req, std::string parentGroupName, std::string newGroupName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to approve creation of the " << newGroupName << " subgroup of " << parentGroupName << " from " << req.remote_endpoint);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
		
	parentGroupName=canonicalizeGroupName(parentGroupName);
	//Only superusers and admins of a Group can alter it
	if(!user.superuser && 
	   store.userStatusInGroup(user.unixName,parentGroupName).state!=GroupMembership::Admin
	   && adminInAnyEnclosingGroup(store,user.unixName,parentGroupName).empty())
		return crow::response(403,generateError("Not authorized"));
	
	newGroupName=canonicalizeGroupName(newGroupName,parentGroupName);
	Group newGroup = store.getGroup(newGroupName);
	if(!newGroup.pending)
		return crow::response(400,generateError("Group already exists"));
	
	GroupRequest newGroupRequest = store.getGroupRequest(newGroupName);
	if(!newGroupRequest)
		return crow::response(404,generateError("Group request not found"));
		
	//ensure that the person who made the request is still a member in good 
	//standing of the parent group
	auto requestorStatus=store.userStatusInGroup(newGroupRequest.requester,parentGroupName);
	if(requestorStatus.state!=GroupMembership::Active &&
	   requestorStatus.state!=GroupMembership::Admin){
		bool success=store.removeGroup(newGroupRequest.name);	
		if (!success)
			log_error("Deleting invalid group request failed");
		return crow::response(400,generateError("User who requested subgroup creation is no longer a member of the enclosing group"));
	}
	
	log_info("Approving creation of subgroup " << newGroupName);
	bool success=store.approveGroupRequest(newGroupName);
	
	if(store.userStatusInGroup(newGroupRequest.requester,parentGroupName).state!=GroupMembership::NonMember){
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
	}
	
	if(!success)
		return crow::response(500, generateError("Storing group request approval failed"));
	
	//inform the person who made the request
	User requestingUser=store.getUser(newGroupRequest.requester);
	if(requestingUser){
		EmailClient::Email message;
		message.fromAddress="noreply@api.ci-connect.net";
		message.toAddresses={requestingUser.email};
		message.subject="CI-Connect group creation request approved";
		message.body="This is an automatic notification that your request to create the group, "+
		newGroupRequest.displayName+" ("+newGroupRequest.name+
		") has been approved and you are now an administrator of this group.";
		store.getEmailClient().sendEmail(message);
	}
	
	return(crow::response(200));
}

crow::response denySubgroupRequest(PersistentStore& store, const crow::request& req, std::string parentGroupName, std::string newGroupName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to deny creation of the " << newGroupName << " subgroup of " << parentGroupName << " from " << req.remote_endpoint);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
		
	parentGroupName=canonicalizeGroupName(parentGroupName);
	//Only superusers and admins of a Group can alter it
	if(!user.superuser && 
	   store.userStatusInGroup(user.unixName,parentGroupName).state!=GroupMembership::Admin
	   && adminInAnyEnclosingGroup(store,user.unixName,parentGroupName).empty())
		return crow::response(403,generateError("Not authorized"));
	
	newGroupName=canonicalizeGroupName(newGroupName);
	GroupRequest newGroupRequest = store.getGroupRequest(newGroupName);
	if(!newGroupRequest)
		return crow::response(404,generateError("Group request not found"));
		
	rapidjson::Document body;
	try{
		if(!req.body.empty())
			body.Parse(req.body.c_str());
	}catch(std::runtime_error& err){
		return crow::response(400,generateError("Invalid JSON in request body"));
	}
	std::string message;
	if(body.IsObject() && body.HasMember("message") && body["message"].IsString())
		message=body["message"].GetString();
	
	bool success = store.removeGroup(newGroupName);
	
	if (!success)
		return crow::response(500, generateError("Deleting group request failed"));
	
	//inform the person who made the request
	User requestingUser=store.getUser(newGroupRequest.requester);
	if(requestingUser){
		EmailClient::Email mail;
		mail.fromAddress="noreply@api.ci-connect.net";
		mail.toAddresses={requestingUser.email};
		mail.subject="CI-Connect group creation request denied";
		mail.body="This is an automatic notification that your request to create the group, "+
		newGroupRequest.displayName+" ("+newGroupRequest.name+
		") has been denied by the enclosing group administrators.";
		if(!message.empty())
			mail.body+="\n\nThe following reason was given: \""+message+"\"";
		store.getEmailClient().sendEmail(mail);
	}
	
	return(crow::response(200));
}

crow::response getGroupAttribute(PersistentStore& store, const crow::request& req, std::string groupName, std::string attributeName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to fetch secondary attribute " << attributeName << " of group " << groupName << " from " << req.remote_endpoint);
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
	log_info(user << " requested to set secondary attribute " << attributeName << " for group " << groupName << " from " << req.remote_endpoint);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
		
	groupName=canonicalizeGroupName(groupName);
	//Only superusers and admins of a Group can alter it
	if(!user.superuser && 
	   store.userStatusInGroup(user.unixName,groupName).state!=GroupMembership::Admin
	   && adminInAnyEnclosingGroup(store,user.unixName,groupName).empty())
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
	log_info(user << " requested to delete secondary attribute " << attributeName << " from group " << groupName << " from " << req.remote_endpoint);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	
	groupName=canonicalizeGroupName(groupName);
	//Only superusers and admins of a Group can alter it
	if(!user.superuser && 
	   store.userStatusInGroup(user.unixName,groupName).state!=GroupMembership::Admin
	   && adminInAnyEnclosingGroup(store,user.unixName,groupName).empty())
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
