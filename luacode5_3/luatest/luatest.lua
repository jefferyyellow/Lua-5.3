function add(...)
	return {...};
end
-- ��ӡȫ�ֱ���
print(global_c_write_value)

local a={1,2,3,4}
a[5]=5
a[4]=nil
print(#a)

-- ����ȫ�ֱ���
global_c_read_val = "------test_global_value------"

function TestVarParam(a,b, ... )
	print(a)
	print(b)
	print(...)
end


-- xpcall(1,2,3,4,5,6,7,8,9,10)

TestVarParam(1,2,3,4,5,6,7)

print("ȫ�֣�")
a = os.clock()
for i = 1,10000000 do
local x = math.sin(i)
end
b = os.clock()
print(b-a)



print("�ֲ���")
a = os.clock()
local sin = math.sin
for i = 1,10000000 do
local x = sin(i)
end
b = os.clock()
print(b-a)