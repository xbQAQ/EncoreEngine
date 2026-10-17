#pragma once

namespace render {
// 编辑器控制参数独立于渲染后端，各 Pass 只将所需设置写入自己的 GPU 快照。
struct RenderSettings {
    unsigned pipeline{1}; // 光栅管线为 0，光追管线为 1；底层调用方保留原有光追默认值。
    unsigned gi{2}; // 路径追踪为 0，SSGI 为 1，ReSTIR GI 为 2。
    unsigned backend{}; // 软件 BVH 为 0，DXR 内联求交为 1。
    unsigned samples{1}; // 光追每像素的新路径数或屏幕间接光预算；不控制光栅直接光。
    bool reset{true}; // 显式请求清空累积历史。
    bool temporalReuse{true}; // 控制时间采样复用与辐亮度重建。
    bool spatialReuse{true}; // 控制 ReSTIR 空间 reservoir 复用。
    bool shadows{true}; // 控制几何可见性阴影。
    unsigned shadowMode{}; // 射线阴影为 0，PCF 为 1，PCSS 为 2，VSM 为 3。
    float shadowBias{0.003f}; // 阴影深度比较的世界空间正偏移。
    float shadowNormalBias{0.005f}; // 阴影投影前沿表面法线的米制偏移。
    float shadowRadius{1.5f}; // PCF 核的基础半径，以阴影纹素计。
    float vsmMinVariance{0.00001f}; // 切比雪夫上界使用的最小归一化深度方差。
    float vsmBleedReduction{0.2f}; // 抑制 VSM 漏光的重映射阈值。
    bool autoExposure{}; // 启用稀疏亮度计量的自动曝光。
    bool simulate{}; // 控制编辑器中的 ECS 模拟更新。
    float exposure{}; // 手动曝光补偿，单位为 EV。
    float bloom{0.06f}; // 最终显示变换中的泛光贡献。
    float giIntensity{1}; // 混合管线的间接辐亮度乘数。
    float mediumDensity{}; // 均匀介质每米消光系数。
    float phaseG{0.2f}; // Henyey-Greenstein 相函数的各向异性参数。
    int pathVertices{5}; // 含主交点的最大表面顶点数。
    int candidates{2}; // 每个接收点的新 ReSTIR 候选数量。
    int maxHistory{32}; // reservoir 有效样本数量上限。
    int spatialRadius{16}; // 空间复用半径，单位为像素。
    int debugView{}; // 最终画面为 0，reservoir M 为 1，权重 W 为 2，法线为 3。
    bool ssao{}; // 启用屏幕空间环境遮蔽。
    float ssaoIntensity{1}; // 近场漫反射遮蔽强度。
    float ssaoRadius{0.5f}; // 环境遮蔽的世界空间采样半径，单位为米。
    bool ssr{}; // 启用屏幕空间镜面反射。
    float ssrIntensity{1}; // 屏幕空间反射的混合强度。
    float ssrDistance{6}; // 反射射线的最大世界空间行进距离，单位为米。
    bool ssil{}; // 启用屏幕空间间接漫反射照明。
    float ssilIntensity{1}; // 屏幕空间漫反射反弹的强度。
    bool sss{}; // 启用按材质次表面权重控制的屏幕空间扩散。
    float sssIntensity{1}; // 次表面扩散混合强度。
    float sssRadius{0.05f}; // 次表面散射世界空间扩散半径，单位为米。
    bool sky{}; // 启用解析天空背景与射线未命中时的环境辐亮度。
    float skyIntensity{1}; // 天空环境辐亮度强度。
};
}
