print "aaa"
base = CBase()
var = base:IsBase()
print(var)

test = CTest(4)
var = test:IsTest()
print(var)

var = test.mValue
print(var)

print(test:RetMul(8))
print(test:GetValue())
test:SetValue(11);
print(test.mValue)
test.mBaseValue = 20
print(test.mBaseValue)
print("-----")
print(test:GetBaseValue());

object = test:GetObject()
print(object:GetValue())
object:SetValue(620)
print(object:GetValue());
print("-*******-")

object = test:GetObject()
print(object:GetValue())

paramObject(test)
paramObject(test)
changeValue(test:GetArray())
print(test:GetArrayByIndex(0))
print(test:GetArrayByIndex(1))
print(test:GetArrayByIndex(2))
paramObject(test)
