
print 'Test vectors in lua:'

config = {
  type = 'Ez',
  unit = 'm',
  mesh_edge_length = 1e9,
  mesh_refines = 0,
  excited_port = 1,
  frequency = 60e9,
  depth = 0,
  cd = Rectangle(-0.5, -0.5, 1.5, 1.5)
}

function PrintVector(v)
  local s = ''
  for i = 1,#v do
    s = s .. string.format('%g ', v[i])
  end
  print('Vector:', s)
  return s
end

function Try(f, expected_status)
  local status,message = pcall(f)
  if not status then
    print(message)
  end
  assert(status == expected_status)
end

-- Creation, sizing and assignment.
v = Vector()
assert(#v == 0)
v:Resize(5)
assert(#v == 5)
assert(PrintVector(v) == '0 0 0 0 0 ')
v[2] = 3      assert(PrintVector(v) == '0 3 0 0 0 ')
v[5] = 9.1    assert(PrintVector(v) == '0 3 0 0 9.1 ')
v[1] = -4.4   assert(PrintVector(v) == '-4.4 3 0 0 9.1 ')
v:Resize(4)
assert(#v == 4)
assert(PrintVector(v) == '0 0 0 0 ')

-- Arithmetic.
a = Vector()
b = Vector()
a:Resize(4)
b:Resize(4)
for i = 1,4 do
  a[i] = 2 + i
  b[i] = 1 + i*2
end
assert(PrintVector(a) == '3 4 5 6 ')
assert(PrintVector(b) == '3 5 7 9 ')
c = -a      assert(PrintVector(c) == '-3 -4 -5 -6 ')
c = a + b   assert(PrintVector(c) == '6 9 12 15 ')
c = a - b   assert(PrintVector(c) == '0 -1 -2 -3 ')
c = a * b   assert(PrintVector(c) == '9 20 35 54 ')
c = a / b   assert(PrintVector(c) == '1 0.8 0.714286 0.666667 ')
c = a ^ b   assert(PrintVector(c) == '27 1024 78125 1.00777e+07 ')

c = a + 2   assert(PrintVector(c) == '5 6 7 8 ')
c = a - 2   assert(PrintVector(c) == '1 2 3 4 ')
c = a * 2   assert(PrintVector(c) == '6 8 10 12 ')
c = a / 2   assert(PrintVector(c) == '1.5 2 2.5 3 ')
c = a ^ 2   assert(PrintVector(c) == '9 16 25 36 ')

c = 2 + a   assert(PrintVector(c) == '5 6 7 8 ')
c = 2 - a   assert(PrintVector(c) == '-1 -2 -3 -4 ')
c = 2 * a   assert(PrintVector(c) == '6 8 10 12 ')
c = 2 / a   assert(PrintVector(c) == '0.666667 0.5 0.4 0.333333 ')
c = 2 ^ a   assert(PrintVector(c) == '8 16 32 64 ')

-- One-argument math functions.
a:Resize(4)
for i = 1,4 do
  a[i] = -2 - i
end
assert(PrintVector(a) == '-3 -4 -5 -6 ')
assert(PrintVector(vec.abs(a)) == '3 4 5 6 ')
for i = 1,4 do
  a[i] = i
end
assert(PrintVector(vec.acos(a / 10)) == '1.47063 1.36944 1.2661 1.15928 ')
assert(PrintVector(vec.asin(a / 10)) == '0.100167 0.201358 0.304693 0.411517 ')
assert(PrintVector(vec.atan(a / 10)) == '0.0996687 0.197396 0.291457 0.380506 ')
assert(PrintVector(vec.ceil(a)) == '1 2 3 4 ')
assert(PrintVector(vec.ceil(a + 0.1)) == '2 3 4 5 ')
assert(PrintVector(vec.cos(a)) == '0.540302 -0.416147 -0.989992 -0.653644 ')
assert(PrintVector(vec.exp(a)) == '2.71828 7.38906 20.0855 54.5982 ')
assert(PrintVector(vec.floor(a)) == '1 2 3 4 ')
assert(PrintVector(vec.floor(a + 0.1)) == '1 2 3 4 ')
assert(PrintVector(vec.floor(a - 0.1)) == '0 1 2 3 ')
assert(PrintVector(vec.log(a)) == '0 0.693147 1.09861 1.38629 ')
assert(PrintVector(vec.log10(a)) == '0 0.30103 0.477121 0.60206 ')
assert(PrintVector(vec.sin(a)) == '0.841471 0.909297 0.14112 -0.756802 ')
assert(PrintVector(vec.sqrt(a)) == '1 1.41421 1.73205 2 ')
assert(PrintVector(vec.tan(a)) == '1.55741 -2.18504 -0.142547 1.15782 ')

-- Two-argument math functions.
a:Resize(4)
b:Resize(4)
for i = 1,4 do
  a[i] = i - 1.5
  b[i] = i*i
end
assert(PrintVector(vec.atan2(a,b)) == '-0.463648 0.124355 0.165149 0.154997 ')
for i = 1,4 do
  a[i] = i*5
  b[i] = i*3
end
assert(PrintVector(vec.fmod(a,b)) == '2 4 6 8 ')

-- Vector comparison functions.
a:Resize(9)     -- a = 1 1 1 2 2 2 3 3 3
b:Resize(9)     -- b = 1 2 3 1 2 3 1 2 3
for i = 1,3 do
  for j = 1,3 do
    a[i*3+j-3] = i
    b[i*3+j-3] = j
  end
end
assert(PrintVector(vec.eq(a,b)) == '1 0 0 0 1 0 0 0 1 ')
assert(PrintVector(vec.ne(a,b)) == '0 1 1 1 0 1 1 1 0 ')
assert(PrintVector(vec.lt(a,b)) == '0 1 1 0 0 1 0 0 0 ')
assert(PrintVector(vec.ge(a,b)) == '1 0 0 1 1 0 1 1 1 ')
assert(PrintVector(vec.gt(a,b)) == '0 0 0 1 0 0 1 1 0 ')
assert(PrintVector(vec.le(a,b)) == '1 1 1 0 1 1 0 0 1 ')

-- Check that incorrect indexes generate errors.
a:Resize(4)
Try(function() print(a[2]) end, true)
Try(function() print(a[5]) end, false)
Try(function() print(a[-1]) end, false)
Try(function() print(a[1.1]) end, false)
Try(function() print(a['foo']) end, false)
Try(function() print(a[{}]) end, false)
Try(function() print(a[function() end]) end, false)
Try(function() a[2] = 1 end, true)
Try(function() a[5] = 1 end, false)
Try(function() a[-1] = 1 end, false)
Try(function() a[1.1] = 1 end, false)

-- Check that bad arguments to operators generate errors.
a:Resize(4)
b:Resize(3)
Try(function() c = a + a end, true)
Try(function() c = a + b end, false)
Try(function() c = a + 'foo' end, false)
Try(function() c = 'foo' + a end, false)

-- Check that bad arguments to other functions generate errors.
Try(function() c = vec.abs(a) end, true)
Try(function() c = vec.abs(1) end, false)
Try(function() c = vec.abs('foo') end, false)
Try(function() c = vec.abs() end, false)
Try(function() c = vec.abs(a,b) end, false)
Try(function() a:Resize(-1) end, false)
Try(function() a:Resize(2.1) end, false)
Try(function() a:Resize(0) end, true)
Try(function() a:Resize(4) end, true)
