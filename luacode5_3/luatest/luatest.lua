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