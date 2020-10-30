local pb = require "pb"
assert(pb.loadfile "loginmessage.pb") 

print("Hello world")

function printfMsg(data)
	local msg = assert(pb.decode("CLoginRequest", data))

	print(msg.Name)
	print(msg.ChannelID)
	print(msg.ServerID)
end