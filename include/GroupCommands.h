#ifndef SLATE_GroupCOMMANDS_H
#define SLATE_GroupCOMMANDS_H

#include "crow.h"
#include "Entities.h"
#include "PersistentStore.h"

///List currently groups which exist
crow::response listGroups(PersistentStore& store, const crow::request& req);
///Register a new group
crow::response createGroup(PersistentStore& store, const crow::request& req, 
                           std::string parentGroupName, std::string newGroupName);
///Get a Group's information
///\param groupID the Group to look up
crow::response getGroupInfo(PersistentStore& store, const crow::request& req, std::string groupName);
///Change a Group's information
///\param groupID the Group to update
crow::response updateGroup(PersistentStore& store, const crow::request& req, std::string groupName);
crow::response updateGroupRequest(PersistentStore& store, const crow::request& req, std::string groupName);
///Delete a group
///\param groupID the Group to destroy
crow::response deleteGroup(PersistentStore& store, const crow::request& req, std::string groupName);
///List the users who belong to a group
///\param groupID the Group to list
crow::response listGroupMembers(PersistentStore& store, const crow::request& req, std::string groupName);

crow::response getGroupMemberStatus(PersistentStore& store, const crow::request& req, const std::string& userID, std::string groupName);

crow::response getSubgroups(PersistentStore& store, const crow::request& req, std::string groupName);

crow::response getSubgroupRequests(PersistentStore& store, const crow::request& req, std::string groupName);

crow::response approveSubgroupRequest(PersistentStore& store, const crow::request& req, std::string parentGroupName, std::string newGroupName);

crow::response denySubgroupRequest(PersistentStore& store, const crow::request& req, std::string parentGroupName, std::string newGroupName);

crow::response getGroupAttribute(PersistentStore& store, const crow::request& req, std::string groupName, std::string attributeName);

crow::response setGroupAttribute(PersistentStore& store, const crow::request& req, std::string groupName, std::string attributeName);

crow::response deleteGroupAttribute(PersistentStore& store, const crow::request& req, std::string groupName, std::string attributeName);

crow::response getScienceFields(PersistentStore& store, const crow::request& req);

std::string canonicalizeGroupName(std::string name, const std::string& enclosingGroup="root");

///\pre groupName should be in canonical form
std::string enclosingGroup(const std::string& groupName);

#endif //SLATE_GroupCOMMANDS_H
