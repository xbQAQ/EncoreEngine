-- 每次模拟更新通过带代际校验的 ECS API 旋转当前实体。
function update(self, dt)
    local x, y, z = world.rotation(self)
    world.set_rotation(self, x, y + 30 * dt, z)
end
