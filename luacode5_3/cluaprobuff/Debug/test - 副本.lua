local pb = require "pb"
assert(pb.loadfile "loginmessage.pb") 

print("Hello world")


local loginRequest = {
	Name = "hjh",
	ChannelID = 2455,
	ServerID = 23456
}

local data = assert(pb.encode("CLoginRequest", loginRequest))

local msg = assert(pb.decode("CLoginRequest", data))

print(msg.Name)
print(msg.ChannelID)
print(msg.ServerID)