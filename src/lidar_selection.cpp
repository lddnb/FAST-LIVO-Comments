#include "lidar_selection.h"

namespace lidar_selection {

LidarSelector::LidarSelector(const int gridsize, SparseMap* sparsemap ): grid_size(gridsize), sparse_map(sparsemap)
{
    downSizeFilter.setLeafSize(0.2, 0.2, 0.2);
    G = Matrix<double, DIM_STATE, DIM_STATE>::Zero();
    H_T_H = Matrix<double, DIM_STATE, DIM_STATE>::Zero();
    Rli = M3D::Identity();
    Rci = M3D::Identity();
    Rcw = M3D::Identity();
    Jdphi_dR = M3D::Identity();
    Jdp_dt = M3D::Identity();
    Jdp_dR = M3D::Identity();
    Pli = V3D::Zero();
    Pci = V3D::Zero();
    Pcw = V3D::Zero();
    width = 800;
    height = 600;
}

LidarSelector::~LidarSelector() 
{
    delete sparse_map;
    delete sub_sparse_map;
    delete[] grid_num;
    delete[] map_index;
    delete[] map_value;
    unordered_map<int, Warp*>().swap(Warp_map);
    unordered_map<VOXEL_KEY, float>().swap(sub_feat_map);
    unordered_map<VOXEL_KEY, VOXEL_POINTS*>().swap(feat_map);  
}

void LidarSelector::set_extrinsic(const V3D &transl, const M3D &rot)
{
    Pli = -rot.transpose() * transl;
    Rli = rot.transpose();
}

void LidarSelector::init()
{
    sub_sparse_map = new SubSparseMap;
    Rci = sparse_map->Rcl * Rli;
    Pci= sparse_map->Rcl*Pli + sparse_map->Pcl;
    M3D Ric;
    V3D Pic;
    Jdphi_dR = Rci;
    Pic = -Rci.transpose() * Pci;
    M3D tmp;
    tmp << SKEW_SYM_MATRX(Pic);
    Jdp_dR = -Rci * tmp;
    width = cam->width();
    height = cam->height();
    grid_n_width = static_cast<int>(width/grid_size);
    grid_n_height = static_cast<int>(height/grid_size);
    length = grid_n_width * grid_n_height;
    fx = cam->errorMultiplier2();
    fy = cam->errorMultiplier() / (4. * fx);
    grid_num = new int[length];
    map_index = new int[length];
    map_value = new float[length];
    map_dist = (float*)malloc(sizeof(float)*length);
    memset(grid_num, TYPE_UNKNOWN, sizeof(int)*length);
    memset(map_index, 0, sizeof(int)*length);
    memset(map_value, 0, sizeof(float)*length);
    voxel_points_.reserve(length);
    add_voxel_points_.reserve(length);
    patch_size_total = patch_size * patch_size;
    patch_size_half = static_cast<int>(patch_size/2);
    patch_cache.resize(patch_size_total);
    stage_ = STAGE_FIRST_FRAME;
    pg_down.reset(new PointCloudXYZI());
    weight_scale_ = 10;
    weight_function_.reset(new vk::robust_cost::HuberWeightFunction());
    // weight_function_.reset(new vk::robust_cost::TukeyWeightFunction());
    scale_estimator_.reset(new vk::robust_cost::UnitScaleEstimator());
    // scale_estimator_.reset(new vk::robust_cost::MADScaleEstimator());
}

void LidarSelector::reset_grid()
{
    memset(grid_num, TYPE_UNKNOWN, sizeof(int)*length);
    memset(map_index, 0, sizeof(int)*length);
    fill_n(map_dist, length, 10000);
    std::vector<PointPtr>(length).swap(voxel_points_);
    std::vector<V3D>(length).swap(add_voxel_points_);
    voxel_points_.reserve(length);
    add_voxel_points_.reserve(length);
}

/**
 * @brief 投影点对三维点的雅可比矩阵计算, slam14 p.220 (8.16)
 * 
 */
void LidarSelector::dpi(V3D p, MD(2,3)& J) {
    const double x = p[0];
    const double y = p[1];
    const double z_inv = 1./p[2];
    const double z_inv_2 = z_inv * z_inv;
    J(0,0) = fx * z_inv;
    J(0,1) = 0.0;
    J(0,2) = -fx * x * z_inv_2;
    J(1,0) = 0.0;
    J(1,1) = fy * z_inv;
    J(1,2) = -fy * y * z_inv_2;
}

float LidarSelector::CheckGoodPoints(cv::Mat img, V2D uv)
{
    const float u_ref = uv[0];
    const float v_ref = uv[1];
    const int u_ref_i = floorf(uv[0]); 
    const int v_ref_i = floorf(uv[1]);
    const float subpix_u_ref = u_ref-u_ref_i;
    const float subpix_v_ref = v_ref-v_ref_i;
    uint8_t* img_ptr = (uint8_t*) img.data + (v_ref_i)*width + (u_ref_i);
    float gu = 2*(img_ptr[1] - img_ptr[-1]) + img_ptr[1-width] - img_ptr[-1-width] + img_ptr[1+width] - img_ptr[-1+width];
    float gv = 2*(img_ptr[width] - img_ptr[-width]) + img_ptr[width+1] - img_ptr[-width+1] + img_ptr[width-1] - img_ptr[-width-1];
    return fabs(gu)+fabs(gv);
}

/**
 * @brief 从图像中提取指定位置的 patch，并进行插值
 * 
 * @param img 
 * @param pc 
 * @param patch_tmp 
 * @param level 
 */
void LidarSelector::getpatch(cv::Mat img, V2D pc, float* patch_tmp, int level) 
{
    const float u_ref = pc[0];
    const float v_ref = pc[1];
    const int scale =  (1<<level);
    const int u_ref_i = floorf(pc[0]/scale)*scale; 
    const int v_ref_i = floorf(pc[1]/scale)*scale;
    const float subpix_u_ref = (u_ref-u_ref_i)/scale;
    const float subpix_v_ref = (v_ref-v_ref_i)/scale;
    const float w_ref_tl = (1.0-subpix_u_ref) * (1.0-subpix_v_ref);
    const float w_ref_tr = subpix_u_ref * (1.0-subpix_v_ref);
    const float w_ref_bl = (1.0-subpix_u_ref) * subpix_v_ref;
    const float w_ref_br = subpix_u_ref * subpix_v_ref;
    for (int x=0; x<patch_size; x++) 
    {
        uint8_t* img_ptr = (uint8_t*) img.data + (v_ref_i-patch_size_half*scale+x*scale)*width + (u_ref_i-patch_size_half*scale);
        for (int y=0; y<patch_size; y++, img_ptr+=scale)
        {
            patch_tmp[patch_size_total*level+x*patch_size+y] = w_ref_tl*img_ptr[0] + w_ref_tr*img_ptr[scale] + w_ref_bl*img_ptr[scale*width] + w_ref_br*img_ptr[scale*width+scale];
        }
    }
}

/**
 * @brief 将激光雷达点投影到当前帧的图像上，计算这些点的角点得分，并根据得分选择最佳点加入视觉地图
 * 
 * @param img 
 * @param pg 
 */
void LidarSelector::addSparseMap(cv::Mat img, PointCloudXYZI::Ptr pg) 
{
    // double t0 = omp_get_wtime();
    reset_grid();

    // double t_b1 = omp_get_wtime() - t0;
    // t0 = omp_get_wtime();
    
    // 遍历点云
    for (int i=0; i<pg->size(); i++) 
    {
        V3D pt(pg->points[i].x, pg->points[i].y, pg->points[i].z);
        V2D pc(new_frame_->w2c(pt));
        if(new_frame_->cam_->isInFrame(pc.cast<int>(), (patch_size_half+1)*8)) // 20px is the patch size in the matcher
        {
            int index = static_cast<int>(pc[0]/grid_size)*grid_n_height + static_cast<int>(pc[1]/grid_size);
            // float cur_value = CheckGoodPoints(img, pc);
            // 计算当前点的角点得分
            float cur_value = vk::shiTomasiScore(img, pc[0], pc[1]);

            // 每个网格取最大角点得分，以及对应的点
            if (cur_value > map_value[index]) //&& (grid_num[index] != TYPE_MAP || map_value[index]<=10)) //! only add in not occupied grid
            {
                map_value[index] = cur_value;
                add_voxel_points_[index] = pt;
                grid_num[index] = TYPE_POINTCLOUD;
            }
        }
    }

    // double t_b2 = omp_get_wtime() - t0;
    // t0 = omp_get_wtime();
    
    int add=0;
    for (int i=0; i<length; i++) 
    {
        if (grid_num[i]==TYPE_POINTCLOUD)// && (map_value[i]>=10)) //! debug
        {
            V3D pt = add_voxel_points_[i];
            V2D pc(new_frame_->w2c(pt));

            PointPtr pt_new(new Point(pt));
            Vector3d f = cam->cam2world(pc);
            // 地图点的特征：在图像上投影的像素坐标、点云方向向量、图像帧位姿、角点得分
            FeaturePtr ftr_new(new Feature(pc, f, new_frame_->T_f_w_, map_value[i], 0));
            ftr_new->img = new_frame_->img_pyr_[0];
            // ftr_new->ImgPyr.resize(5);
            // for(int i=0;i<5;i++) ftr_new->ImgPyr[i] = new_frame_->img_pyr_[i];
            ftr_new->id_ = new_frame_->id_;

            pt_new->addFrameRef(ftr_new);
            pt_new->value = map_value[i];
            // 往相应的体素中添加地图点
            AddPoint(pt_new);
            add += 1;
        }
    }

    // double t_b3 = omp_get_wtime() - t0;

    printf("[ VIO ]: Add %d 3D points.\n", add);
    // printf("pg.size: %d \n", pg->size());
    // printf("B1. : %.6lf \n", t_b1);
    // printf("B2. : %.6lf \n", t_b2);
    // printf("B3. : %.6lf \n", t_b3);
}

/**
 * @brief 往视觉地图的体素中添加新的点
 * 
 * @param pt_new 
 */
void LidarSelector::AddPoint(PointPtr pt_new)
{
    V3D pt_w(pt_new->pos_[0], pt_new->pos_[1], pt_new->pos_[2]);
    double voxel_size = 0.5;
    float loc_xyz[3];
    for(int j=0; j<3; j++)
    {
      loc_xyz[j] = pt_w[j] / voxel_size;
      if(loc_xyz[j] < 0)
      {
        loc_xyz[j] -= 1.0;
      }
    }
    VOXEL_KEY position((int64_t)loc_xyz[0], (int64_t)loc_xyz[1], (int64_t)loc_xyz[2]);
    auto iter = feat_map.find(position);
    if(iter != feat_map.end())
    {
      iter->second->voxel_points.push_back(pt_new);
      iter->second->count++;
    }
    else
    {
      VOXEL_POINTS *ot = new VOXEL_POINTS(0);
      ot->voxel_points.push_back(pt_new);
      feat_map[position] = ot;
    }
}

/**
 * @brief 计算仿射矩阵的流程，只是添加了金字塔层级，但是也没用到，默认为0
 * 
 * @param cam 
 * @param px_ref 
 * @param f_ref 
 * @param depth_ref 
 * @param T_cur_ref 
 * @param level_ref 
 * @param pyramid_level 
 * @param halfpatch_size 
 * @param A_cur_ref 
 */
void LidarSelector::getWarpMatrixAffine(
    const vk::AbstractCamera& cam,
    const Vector2d& px_ref,
    const Vector3d& f_ref,
    const double depth_ref,
    const SE3& T_cur_ref,
    const int level_ref,    // the corresponding pyrimid level of px_ref
    const int pyramid_level,
    const int halfpatch_size,
    Matrix2d& A_cur_ref)
{
  // Compute affine warp matrix A_ref_cur
  const Vector3d xyz_ref(f_ref*depth_ref);
  Vector3d xyz_du_ref(cam.cam2world(px_ref + Vector2d(halfpatch_size,0)*(1<<level_ref)*(1<<pyramid_level)));
  Vector3d xyz_dv_ref(cam.cam2world(px_ref + Vector2d(0,halfpatch_size)*(1<<level_ref)*(1<<pyramid_level)));
//   Vector3d xyz_du_ref(cam.cam2world(px_ref + Vector2d(halfpatch_size,0)*(1<<level_ref)));
//   Vector3d xyz_dv_ref(cam.cam2world(px_ref + Vector2d(0,halfpatch_size)*(1<<level_ref)));
  xyz_du_ref *= xyz_ref[2]/xyz_du_ref[2];
  xyz_dv_ref *= xyz_ref[2]/xyz_dv_ref[2];
  const Vector2d px_cur(cam.world2cam(T_cur_ref*(xyz_ref)));
  const Vector2d px_du(cam.world2cam(T_cur_ref*(xyz_du_ref)));
  const Vector2d px_dv(cam.world2cam(T_cur_ref*(xyz_dv_ref)));
  A_cur_ref.col(0) = (px_du - px_cur)/halfpatch_size;
  A_cur_ref.col(1) = (px_dv - px_cur)/halfpatch_size;
}

/**
 * @brief 利用仿射变换矩阵将参考帧的patch变换到当前帧图像上，并计算变换后的像素值
 * 
 * @param A_cur_ref 
 * @param img_ref 
 * @param px_ref 
 * @param level_ref 
 * @param search_level 
 * @param pyramid_level 
 * @param halfpatch_size 
 * @param patch 
 */
void LidarSelector::warpAffine(
    const Matrix2d& A_cur_ref,
    const cv::Mat& img_ref,
    const Vector2d& px_ref,
    const int level_ref,
    const int search_level,
    const int pyramid_level,
    const int halfpatch_size,
    float* patch)
{
  const int patch_size = halfpatch_size*2 ;
  // 逆向映射 Inverse Mapping 的操作，可以对ref图像上的像素做插值
  const Matrix2f A_ref_cur = A_cur_ref.inverse().cast<float>();
  if(isnan(A_ref_cur(0,0)))
  {
    printf("Affine warp is NaN, probably camera has no translation\n"); // TODO
    return;
  }
//   Perform the warp on a larger patch.
//   float* patch_ptr = patch;
//   const Vector2f px_ref_pyr = px_ref.cast<float>() / (1<<level_ref) / (1<<pyramid_level);
//   const Vector2f px_ref_pyr = px_ref.cast<float>() / (1<<level_ref);
  for (int y=0; y<patch_size; ++y)
  {
    for (int x=0; x<patch_size; ++x)//, ++patch_ptr)
    {
      // P[patch_size_total*level + x*patch_size+y]
      Vector2f px_patch(x-halfpatch_size, y-halfpatch_size);
      // 先根据最佳搜索层级调整特征尺度，然后再进行逐层的金字塔patch计算
      px_patch *= (1<<search_level);
      px_patch *= (1<<pyramid_level);
      const Vector2f px(A_ref_cur*px_patch + px_ref.cast<float>());
      if (px[0]<0 || px[1]<0 || px[0]>=img_ref.cols-1 || px[1]>=img_ref.rows-1)
        patch[patch_size_total*pyramid_level + y*patch_size+x] = 0;
        // *patch_ptr = 0;
      else
        // 原始分辨率的参考帧图像上进行双线性插值，获取精确的像素值
        patch[patch_size_total*pyramid_level + y*patch_size+x] = (float) vk::interpolateMat_8u(img_ref, px[0], px[1]);
        // *patch_ptr = (uint8_t) vk::interpolateMat_8u(img_ref, px[0], px[1]);
    }
  }
}

/**
 * @brief 计算归一化互相关系数（NCC），用于衡量两个图像patch之间的相似度
 * 
 * @param ref_patch 
 * @param cur_patch 
 * @param patch_size 
 * @return double 
 */
double LidarSelector::NCC(float* ref_patch, float* cur_patch, int patch_size)
{    
    double sum_ref = std::accumulate(ref_patch, ref_patch + patch_size, 0.0);
    double mean_ref =  sum_ref / patch_size;

    double sum_cur = std::accumulate(cur_patch, cur_patch + patch_size, 0.0);
    double mean_curr =  sum_cur / patch_size;

    double numerator = 0, demoniator1 = 0, demoniator2 = 0;
    for (int i = 0; i < patch_size; i++) 
    {
        double n = (ref_patch[i] - mean_ref) * (cur_patch[i] - mean_curr);
        numerator += n;
        demoniator1 += (ref_patch[i] - mean_ref) * (ref_patch[i] - mean_ref);
        demoniator2 += (cur_patch[i] - mean_curr) * (cur_patch[i] - mean_curr);
    }
    return numerator / sqrt(demoniator1 * demoniator2 + 1e-10);
}

/**
 * @brief 根据仿射变换矩阵的行列式确定最佳的金字塔搜索层级
 * 
 * @param A_cur_ref 
 * @param max_level 
 * @return int 
 */
int LidarSelector::getBestSearchLevel(
    const Matrix2d& A_cur_ref,
    const int max_level)
{
  // Compute patch level in other image
  int search_level = 0;
  double D = A_cur_ref.determinant(); // 行列式几何意义：面积的缩放比例

  // 当缩放比例大于3时，往上层搜索
  while(D > 3.0 && search_level < max_level)
  {
    search_level += 1;
    D *= 0.25;
  }
  return search_level;
}

#ifdef FeatureAlign
void LidarSelector::createPatchFromPatchWithBorder(float* patch_with_border, float* patch_ref)
{
  float* ref_patch_ptr = patch_ref;
  for(int y=1; y<patch_size+1; ++y, ref_patch_ptr += patch_size)
  {
    float* ref_patch_border_ptr = patch_with_border + y*(patch_size+2) + 1;
    for(int x=0; x<patch_size; ++x)
      ref_patch_ptr[x] = ref_patch_border_ptr[x];
  }
}
#endif

/**
 * @brief  传入当前帧图像和上一帧的LIDAR点云，计算深度图以及点云对应的视觉点图体素，从体素中选择与当前帧观测角度最相近的观测作为参考帧，
 *         然后计算参考帧与当前帧之间的affine变换，计算参考帧变换至当前帧对应的patch，最后计算这个patch和当前帧观测到的patch之间的像素误差
 * 
 * @param img 
 * @param pg 
 */
void LidarSelector::addFromSparseMap(cv::Mat img, PointCloudXYZI::Ptr pg)
{
    if(feat_map.size()<=0) return;
    // double ts0 = omp_get_wtime();

    pg_down->reserve(feat_map.size());
    downSizeFilter.setInputCloud(pg);
    downSizeFilter.filter(*pg_down);
    
    reset_grid();
    memset(map_value, 0, sizeof(float)*length);

    sub_sparse_map->reset();
    deque< PointPtr >().swap(sub_map_cur_frame_);

    float voxel_size = 0.5;
    
    unordered_map<VOXEL_KEY, float>().swap(sub_feat_map);
    unordered_map<int, Warp*>().swap(Warp_map);

    // 计算深度图
    cv::Mat depth_img = cv::Mat::zeros(height, width, CV_32FC1);
    float* it = (float*)depth_img.data;

    double t_insert, t_depth, t_position;
    t_insert=t_depth=t_position=0;

    int loc_xyz[3];

    // printf("A0. initial depthmap: %.6lf \n", omp_get_wtime() - ts0);
    // double ts1 = omp_get_wtime();

    for(int i=0; i<pg_down->size(); i++)
    {
        // Transform Point to world coordinate
        V3D pt_w(pg_down->points[i].x, pg_down->points[i].y, pg_down->points[i].z);

        // Determine the key of hash table      
        for(int j=0; j<3; j++)
        {
            loc_xyz[j] = floor(pt_w[j] / voxel_size);
        }
        VOXEL_KEY position(loc_xyz[0], loc_xyz[1], loc_xyz[2]);

        // 找出当前点所在的体素位置，并将其添加到sub_feat_map中
        auto iter = sub_feat_map.find(position);
        if(iter == sub_feat_map.end())
        {
            sub_feat_map[position] = 1.0;
        }
                    
        V3D pt_c(new_frame_->w2f(pt_w));

        V2D px;
        if(pt_c[2] > 0)
        {
            px[0] = fx * pt_c[0]/pt_c[2] + cx;
            px[1] = fy * pt_c[1]/pt_c[2] + cy;

            // 检查像素点是否在图像边缘40以内的区域
            if(new_frame_->cam_->isInFrame(px.cast<int>(), (patch_size_half+1)*8))
            {
                float depth = pt_c[2];
                int col = int(px[0]);
                int row = int(px[1]);
                it[width*row+col] = depth;        
            }
        }
    }
    
    // imshow("depth_img", depth_img);
    // printf("A1: %.6lf \n", omp_get_wtime() - ts1);
    // printf("A11. calculate pt position: %.6lf \n", t_position);
    // printf("A12. sub_postion.insert(position): %.6lf \n", t_insert);
    // printf("A13. generate depth map: %.6lf \n", t_depth);
    // printf("A. projection: %.6lf \n", omp_get_wtime() - ts0);
    

    // double t1 = omp_get_wtime();

    // 遍历上面找到的所有体素
    for(auto& iter : sub_feat_map)
    {   
        VOXEL_KEY position = iter.first;
        // double t4 = omp_get_wtime();
        auto corre_voxel = feat_map.find(position);
        // double t5 = omp_get_wtime();

        if(corre_voxel != feat_map.end())
        {
            // 把体素中的所有地图点都拿出来投影到图像上
            std::vector<PointPtr> &voxel_points = corre_voxel->second->voxel_points;
            int voxel_num = voxel_points.size();
            for (int i=0; i<voxel_num; i++)
            {
                PointPtr pt = voxel_points[i];
                if(pt==nullptr) continue;
                V3D pt_cam(new_frame_->w2f(pt->pos_));
                if(pt_cam[2]<0) continue;

                V2D pc(new_frame_->w2c(pt->pos_));

                FeaturePtr ref_ftr;
      
                if(new_frame_->cam_->isInFrame(pc.cast<int>(), (patch_size_half+1)*8)) // 20px is the patch size in the matcher
                {
                    // 计算属于哪个grid
                    int index = static_cast<int>(pc[0]/grid_size)*grid_n_height + static_cast<int>(pc[1]/grid_size);
                    grid_num[index] = TYPE_MAP;
                    // 计算视线观测向量
                    Vector3d obs_vec(new_frame_->pos() - pt->pos_);

                    float cur_dist = obs_vec.norm();
                    //! value 是 shiTomasiScore，也就是角点的得分，得分越高，说明这个角点越明显
                    float cur_value = pt->value;

                    // 为了防止遮挡，选 40x40 的grid中最近的那个点
                    if (cur_dist <= map_dist[index]) 
                    {
                        map_dist[index] = cur_dist;
                        voxel_points_[index] = pt;
                    } 

                    // 更新grid的角点得分
                    if (cur_value >= map_value[index])
                    {
                        map_value[index] = cur_value;
                    }
                }
            }    
        } 
    }
        
    // double t2 = omp_get_wtime();

    // cout<<"B. feat_map.find: "<<t2-t1<<endl;

    double t_2, t_3, t_4, t_5;
    t_2=t_3=t_4=t_5=0;

    // 遍历所有grid
    for (int i=0; i<length; i++) 
    { 
        // 如果grid中有地图点
        if (grid_num[i]==TYPE_MAP) //&& map_value[i]>10)
        {
            // double t_1 = omp_get_wtime();

            PointPtr pt = voxel_points_[i];

            if(pt==nullptr) continue;

            // 图像坐标系下的像素坐标
            V2D pc(new_frame_->w2c(pt->pos_));
            // 相机坐标系下的三维坐标
            V3D pt_cam(new_frame_->w2f(pt->pos_));
   
            // 判断以当前点为中心的patch的深度连续性，即当前点深度与其周围像素的深度差别不应该太大
            bool depth_continous = false;
            for (int u=-patch_size_half; u<=patch_size_half; u++)
            {
                for (int v=-patch_size_half; v<=patch_size_half; v++)
                {
                    // path 中心，跳过
                    if(u==0 && v==0) continue;

                    float depth = it[width*(v+int(pc[1]))+u+int(pc[0])];

                    // 没有深度，跳过
                    if(depth == 0.) continue;

                    double delta_dist = abs(pt_cam[2]-depth);

                    // 如果当前点深度与周围像素的深度差别大于1.5米，则认为当前点深度不连续，后面就不用算了
                    if(delta_dist > 1.5)
                    {                
                        depth_continous = true;
                        break;
                    }
                }
                if(depth_continous) break;
            }
            if(depth_continous) continue;

            // t_2 += omp_get_wtime() - t_1;

            // t_1 = omp_get_wtime();
            
            FeaturePtr ref_ftr;

            // 在当前点的历史观测中，找一个与当前观测方向最近的一个观测作为参考帧，如果都大于60°，就跳过
            if(!pt->getCloseViewObs(new_frame_->pos(), ref_ftr, pc)) continue;

            // t_3 += omp_get_wtime() - t_1;

            // 因为有图像金字塔，缩放两次，加上原始图像，所一共有3个wrap
            std::vector<float> patch_wrap(patch_size_total * 3);

            // patch_wrap = ref_ftr->patch;

            // t_1 = omp_get_wtime();
           
            int search_level;
            Matrix2d A_cur_ref_zero;

            auto iter_warp = Warp_map.find(ref_ftr->id_);
            if(iter_warp != Warp_map.end())
            {
                search_level = iter_warp->second->search_level;
                A_cur_ref_zero = iter_warp->second->A_cur_ref;
            }
            else
            {
                // 计算参考帧->当前帧的仿射变换矩阵
                getWarpMatrixAffine(*cam, ref_ftr->px, ref_ftr->f, (ref_ftr->pos() - pt->pos_).norm(), 
                new_frame_->T_f_w_ * ref_ftr->T_f_w_.inverse(), 0, 0, patch_size_half, A_cur_ref_zero);
                
                // 判断到哪个金字塔层级里面寻找像素对应关系
                search_level = getBestSearchLevel(A_cur_ref_zero, 2);

                Warp *ot = new Warp(search_level, A_cur_ref_zero);
                Warp_map[ref_ftr->id_] = ot;
            }

            // t_4 += omp_get_wtime() - t_1;

            // t_1 = omp_get_wtime();

            // 对三层金字塔实施仿射变换，获取地图点在当前帧图像上的patch
            for(int pyramid_level=0; pyramid_level<=2; pyramid_level++)
            {                
                warpAffine(A_cur_ref_zero, ref_ftr->img, ref_ftr->px, ref_ftr->level, search_level, pyramid_level, patch_size_half, patch_wrap.data());
            }

            // 从当前帧图像中获取当前地图点的patch，但是没用金字塔
            getpatch(img, pc, patch_cache.data(), 0);

            // 计算NCC
            if(ncc_en)
            {
                double ncc = NCC(patch_wrap.data(), patch_cache.data(), patch_size_total);
                if(ncc < ncc_thre) continue;
            }

            // 计算当前地图点的patch与参考帧的patch之间像素误差的平方和
            float error = 0.0;
            for (int ind=0; ind<patch_size_total; ind++) 
            {
                error += (patch_wrap[ind]-patch_cache[ind]) * (patch_wrap[ind]-patch_cache[ind]);
            }
            if(error > outlier_threshold*patch_size_total) continue;
            
            // 用到的地图点存起来，但只用于显示
            sub_map_cur_frame_.push_back(pt);

            sub_sparse_map->propa_errors.push_back(error);
            sub_sparse_map->search_levels.push_back(search_level);
            sub_sparse_map->errors.push_back(error);
            sub_sparse_map->index.push_back(i);  
            sub_sparse_map->voxel_points.push_back(pt);
            sub_sparse_map->patch.push_back(std::move(patch_wrap));
            // t_5 += omp_get_wtime() - t_1;
        }
    }
    // double t3 = omp_get_wtime();
    // cout<<"C. addSubSparseMap: "<<t3-t2<<endl;
    // cout<<"depthcontinuous: C1 "<<t_2<<" C2 "<<t_3<<" C3 "<<t_4<<" C4 "<<t_5<<endl;
    printf("[ VIO ]: choose %d points from sub_sparse_map.\n", int(sub_sparse_map->index.size()));
}

#ifdef FeatureAlign
bool LidarSelector::align2D(
    const cv::Mat& cur_img,
    float* ref_patch_with_border,
    float* ref_patch,
    const int n_iter,
    Vector2d& cur_px_estimate,
    int index)
{
#ifdef __ARM_NEON__
  if(!no_simd)
    return align2D_NEON(cur_img, ref_patch_with_border, ref_patch, n_iter, cur_px_estimate);
#endif

  const int halfpatch_size_ = 4;
  const int patch_size_ = 8;
  const int patch_area_ = 64;
  bool converged=false;

  // compute derivative of template and prepare inverse compositional
  float __attribute__((__aligned__(16))) ref_patch_dx[patch_area_];
  float __attribute__((__aligned__(16))) ref_patch_dy[patch_area_];
  Matrix3f H; H.setZero();

  // compute gradient and hessian
  const int ref_step = patch_size_+2;
  float* it_dx = ref_patch_dx;
  float* it_dy = ref_patch_dy;
  for(int y=0; y<patch_size_; ++y) 
  {
    float* it = ref_patch_with_border + (y+1)*ref_step + 1; 
    for(int x=0; x<patch_size_; ++x, ++it, ++it_dx, ++it_dy)
    {
      Vector3f J;
      J[0] = 0.5 * (it[1] - it[-1]); 
      J[1] = 0.5 * (it[ref_step] - it[-ref_step]); 
      J[2] = 1; 
      *it_dx = J[0];
      *it_dy = J[1];
      H += J*J.transpose(); 
    }
  }
  Matrix3f Hinv = H.inverse();
  float mean_diff = 0;

  // Compute pixel location in new image:
  float u = cur_px_estimate.x();
  float v = cur_px_estimate.y();

  // termination condition
  const float min_update_squared = 0.03*0.03;//0.03*0.03
  const int cur_step = cur_img.step.p[0];
  float chi2 = 0;
  chi2 = sub_sparse_map->propa_errors[index];
  Vector3f update; update.setZero();
  for(int iter = 0; iter<n_iter; ++iter)
  {
    int u_r = floor(u);
    int v_r = floor(v);
    if(u_r < halfpatch_size_ || v_r < halfpatch_size_ || u_r >= cur_img.cols-halfpatch_size_ || v_r >= cur_img.rows-halfpatch_size_)
      break;

    if(isnan(u) || isnan(v)) // TODO very rarely this can happen, maybe H is singular? should not be at corner.. check
      return false;

    // compute interpolation weights
    float subpix_x = u-u_r;
    float subpix_y = v-v_r;
    float wTL = (1.0-subpix_x)*(1.0-subpix_y);
    float wTR = subpix_x * (1.0-subpix_y);
    float wBL = (1.0-subpix_x)*subpix_y;
    float wBR = subpix_x * subpix_y;

    // loop through search_patch, interpolate
    float* it_ref = ref_patch;
    float* it_ref_dx = ref_patch_dx;
    float* it_ref_dy = ref_patch_dy;
    float new_chi2 = 0.0;
    Vector3f Jres; Jres.setZero();
    for(int y=0; y<patch_size_; ++y)
    {
      uint8_t* it = (uint8_t*) cur_img.data + (v_r+y-halfpatch_size_)*cur_step + u_r-halfpatch_size_; 
      for(int x=0; x<patch_size_; ++x, ++it, ++it_ref, ++it_ref_dx, ++it_ref_dy)
      {
        float search_pixel = wTL*it[0] + wTR*it[1] + wBL*it[cur_step] + wBR*it[cur_step+1];
        float res = search_pixel - *it_ref + mean_diff;
        Jres[0] -= res*(*it_ref_dx);
        Jres[1] -= res*(*it_ref_dy);
        Jres[2] -= res;
        new_chi2 += res*res;
      }
    }

    if(iter > 0 && new_chi2 > chi2)
    {
    //   cout << "error increased." << endl;
      u -= update[0];
      v -= update[1];
      break;
    }
    chi2 = new_chi2;

    sub_sparse_map->align_errors[index] = new_chi2;

    update = Hinv * Jres;
    u += update[0];
    v += update[1];
    mean_diff += update[2];

#if SUBPIX_VERBOSE
    cout << "Iter " << iter << ":"
         << "\t u=" << u << ", v=" << v
         << "\t update = " << update[0] << ", " << update[1]
//         << "\t new chi2 = " << new_chi2 << endl;
#endif

    if(update[0]*update[0]+update[1]*update[1] < min_update_squared)
    {
#if SUBPIX_VERBOSE
      cout << "converged." << endl;
#endif
      converged=true;
      break;
    }
  }

  cur_px_estimate << u, v;
  return converged;
}

void LidarSelector::FeatureAlignment(cv::Mat img)
{
    int total_points = sub_sparse_map->index.size();
    if (total_points==0) return;
    memset(align_flag, 0, length);
    int FeatureAlignmentNum = 0;
       
    for (int i=0; i<total_points; i++) 
    {
        bool res;
        int search_level = sub_sparse_map->search_levels[i];
        Vector2d px_scaled(sub_sparse_map->px_cur[i]/(1<<search_level));
        res = align2D(new_frame_->img_pyr_[search_level], sub_sparse_map->patch_with_border[i], sub_sparse_map->patch[i],
                        20, px_scaled, i);
        sub_sparse_map->px_cur[i] = px_scaled * (1<<search_level);
        if(res)
        {
            align_flag[i] = 1;
            FeatureAlignmentNum++;
        }
    }
}
#endif

/**
 * @brief 利用光度误差更新状态
 * 
 * @param img 
 * @param total_residual 
 * @param level 
 * @return float 
 */
float LidarSelector::UpdateState(cv::Mat img, float total_residual, int level) 
{
    int total_points = sub_sparse_map->index.size(); // 计算了误差的patch的size
    if (total_points==0) return 0.;
    StatesGroup old_state = (*state); // 旧状态
    V2D pc; 
    MD(1,2) Jimg; // 像素梯度
    MD(2,3) Jdpi;
    MD(1,3) Jdphi, Jdp, JdR, Jdt;
    VectorXd z;
    // VectorXd R;
    bool EKF_end = false;
    /* Compute J */
    float error=0.0, last_error=total_residual, patch_error=0.0, last_patch_error=0.0, propa_error=0.0;
    // MatrixXd H;
    bool z_init = true;
    const int H_DIM = total_points * patch_size_total; // 所有patch的像素点总数，残差的维度
    
    // K.resize(H_DIM, H_DIM);
    z.resize(H_DIM);
    z.setZero();
    // R.resize(H_DIM);
    // R.setZero();

    // H.resize(H_DIM, DIM_STATE);
    // H.setZero();
    H_sub.resize(H_DIM, 6);
    H_sub.setZero();
    
    for (int iteration=0; iteration<NUM_MAX_ITERATIONS; iteration++) 
    {
        // double t1 = omp_get_wtime();
        double count_outlier = 0;
     
        error = 0.0;
        propa_error = 0.0;
        n_meas_ =0;
        M3D Rwi(state->rot_end);
        V3D Pwi(state->pos_end);
        Rcw = Rci * Rwi.transpose();
        Pcw = -Rci*Rwi.transpose()*Pwi + Pci;
        Jdp_dt = Rci * Rwi.transpose();
        
        M3D p_hat;
        int i;

        for (i=0; i<sub_sparse_map->index.size(); i++) 
        {
            patch_error = 0.0;
            // 确定当前金字塔层级
            int search_level = sub_sparse_map->search_levels[i];
            int pyramid_level = level + search_level;
            const int scale =  (1<<pyramid_level);
            
            PointPtr pt = sub_sparse_map->voxel_points[i];

            if(pt==nullptr) continue;

            V3D pf = Rcw * pt->pos_ + Pcw; // 当前帧相机系下的地图点三维坐标
            pc = cam->world2cam(pf); // 当前帧下的patch中心像素坐标
            // if((level==2 && iteration==0) || (level==1 && iteration==0) || level==0)
            {
                dpi(pf, Jdpi); // ∂u/∂q
                p_hat << SKEW_SYM_MATRX(pf); // ∂q/∂φ，对SE3李代数旋转部分的雅克比
            }
            const float u_ref = pc[0];
            const float v_ref = pc[1];
            const int u_ref_i = floorf(pc[0]/scale)*scale; 
            const int v_ref_i = floorf(pc[1]/scale)*scale;
            const float subpix_u_ref = (u_ref-u_ref_i)/scale;
            const float subpix_v_ref = (v_ref-v_ref_i)/scale;
            const float w_ref_tl = (1.0-subpix_u_ref) * (1.0-subpix_v_ref);
            const float w_ref_tr = subpix_u_ref * (1.0-subpix_v_ref);
            const float w_ref_bl = (1.0-subpix_u_ref) * subpix_v_ref;
            const float w_ref_br = subpix_u_ref * subpix_v_ref;
            
            vector<float> P = sub_sparse_map->patch[i];
            for (int x=0; x<patch_size; x++) 
            {
                uint8_t* img_ptr = (uint8_t*) img.data + (v_ref_i+x*scale-patch_size_half*scale)*width + u_ref_i-patch_size_half*scale;
                for (int y=0; y<patch_size; ++y, img_ptr+=scale) 
                {
                    // if((level==2 && iteration==0) || (level==1 && iteration==0) || level==0)
                    //{
                    // 先用双线性插值计算像素，然后使用中值差分计算像素梯度
                    float du = 0.5f * ((w_ref_tl*img_ptr[scale] + w_ref_tr*img_ptr[scale*2] + w_ref_bl*img_ptr[scale*width+scale] + w_ref_br*img_ptr[scale*width+scale*2])
                                -(w_ref_tl*img_ptr[-scale] + w_ref_tr*img_ptr[0] + w_ref_bl*img_ptr[scale*width-scale] + w_ref_br*img_ptr[scale*width]));
                    float dv = 0.5f * ((w_ref_tl*img_ptr[scale*width] + w_ref_tr*img_ptr[scale+scale*width] + w_ref_bl*img_ptr[width*scale*2] + w_ref_br*img_ptr[width*scale*2+scale])
                                -(w_ref_tl*img_ptr[-scale*width] + w_ref_tr*img_ptr[-scale*width+scale] + w_ref_bl*img_ptr[0] + w_ref_br*img_ptr[scale]));
                    Jimg << du, dv;
                    Jimg = Jimg * (1.0/scale); // 像素梯度除以缩放比例
                    // 对SE3李代数的雅克比 ∂e/∂[p φ] slam14 p.220 (8.15)
                    Jdphi = Jimg * Jdpi * p_hat; // ∂e/∂φ
                    Jdp = -Jimg * Jdpi; // ∂e/∂ρ 
                    //! 从对相机系下的SE3(T_cw)的雅克比转成IMU系下SO3+R3(R_wi+t_wi)的雅克比
                    //! 这里的 ∂φ/∂R、 ∂ρ/∂R、∂ρ/∂t 和实际推导结果差了个负号
                    JdR = Jdphi * Jdphi_dR + Jdp * Jdp_dR; // ∂e/∂R = ∂e/∂φ * ∂φ/∂R + ∂e/∂ρ * ∂ρ/∂R
                    Jdt = Jdp * Jdp_dt; // ∂e/∂t = ∂e/∂ρ * ∂ρ/∂t
                    //}

                    // 计算当前像素点的误差，当前帧-参考帧
                    double res = w_ref_tl*img_ptr[0] + w_ref_tr*img_ptr[scale] + w_ref_bl*img_ptr[scale*width] + w_ref_br*img_ptr[scale*width+scale]  - P[patch_size_total*level + x*patch_size+y];
                    // 保存残差
                    z(i*patch_size_total+x*patch_size+y) = res;
                    // float weight = 1.0;
                    // if(iteration > 0)
                    //     weight = weight_function_->value(res/weight_scale_); 
                    // R(i*patch_size_total+x*patch_size+y) = weight;       
                    patch_error +=  res*res;
                    n_meas_++;
                    // H.block<1,6>(i*patch_size_total+x*patch_size+y,0) << JdR*weight, Jdt*weight;
                    // if((level==2 && iteration==0) || (level==1 && iteration==0) || level==0)
                    // 保存雅克比
                    H_sub.block<1,6>(i*patch_size_total+x*patch_size+y,0) << JdR, Jdt;
                }
            }  

            sub_sparse_map->errors[i] = patch_error;
            error += patch_error;
        }

        // computeH += omp_get_wtime() - t1;

        error = error/n_meas_;

        // double t3 = omp_get_wtime();

        // 上一次优化有效就继续优化，否则取回上一次状态结束优化
        if (error <= last_error) 
        {
            old_state = (*state);
            last_error = error;

            // K = (H.transpose() / img_point_cov * H + state->cov.inverse()).inverse() * H.transpose() / img_point_cov;
            // auto vec = (*state_propagat) - (*state);
            // G = K*H;
            // (*state) += (-K*z + vec - G*vec);

            auto &&H_sub_T = H_sub.transpose();
            H_T_H.block<6,6>(0,0) = H_sub_T * H_sub;
            // 类似fast-lio的K计算方式
            MD(DIM_STATE, DIM_STATE) &&K_1 = (H_T_H + (state->cov / img_point_cov).inverse()).inverse();
            auto &&HTz = H_sub_T * z;
            // K = K_1.block<DIM_STATE,6>(0,0) * H_sub_T;
            auto vec = (*state_propagat) - (*state);
            G.block<DIM_STATE,6>(0,0) = K_1.block<DIM_STATE,6>(0,0) * H_T_H.block<6,6>(0,0);
            auto solution = - K_1.block<DIM_STATE,6>(0,0) * HTz + vec - G.block<DIM_STATE,6>(0,0) * vec.block<6,1>(0,0);
            (*state) += solution;
            auto &&rot_add = solution.block<3,1>(0,0);
            auto &&t_add   = solution.block<3,1>(3,0);

            // 状态增量较小时结束优化
            if ((rot_add.norm() * 57.3f < 0.001f) && (t_add.norm() * 100.0f < 0.001f))
            {
                EKF_end = true;
            }
        }
        else
        {
            (*state) = old_state;
            EKF_end = true;
        }

        // ekf_time += omp_get_wtime() - t3;

        if (iteration==NUM_MAX_ITERATIONS || EKF_end) 
        {
            break;
        }
    }
    return last_error;
} 

void LidarSelector::updateFrameState(StatesGroup state)
{
    M3D Rwi(state.rot_end);
    V3D Pwi(state.pos_end);
    Rcw = Rci * Rwi.transpose();
    Pcw = -Rci*Rwi.transpose()*Pwi + Pci;
    new_frame_->T_f_w_ = SE3(Rcw, Pcw);
}

/**
 * @brief 将地图点在当前帧下的观测与地图点进行关联
 * 
 * @param img 
 */
void LidarSelector::addObservation(cv::Mat img)
{
    int total_points = sub_sparse_map->index.size();
    if (total_points==0) return;

    for (int i=0; i<total_points; i++) 
    {
        PointPtr pt = sub_sparse_map->voxel_points[i];
        if(pt==nullptr) continue;
        V2D pc(new_frame_->w2c(pt->pos_));
        SE3 pose_cur = new_frame_->T_f_w_;
        bool add_flag = false;
        // 筛选条件：当前帧与地图点的上一次观测的位姿差和投影点的像素差大于阈值
        // if (sub_sparse_map->errors[i]<= 100*patch_size_total && sub_sparse_map->errors[i]>0)
        {
            //TODO: condition: distance and view_angle 
            // Step 1: time
            FeaturePtr last_feature =  pt->obs_.back();
            // if(new_frame_->id_ >= last_feature->id_ + 20) add_flag = true;

            // Step 2: delta_pose
            SE3 pose_ref = last_feature->T_f_w_;
            SE3 delta_pose = pose_ref * pose_cur.inverse();
            double delta_p = delta_pose.translation().norm();
            double delta_theta = (delta_pose.rotation_matrix().trace() > 3.0 - 1e-6) ? 0.0 : std::acos(0.5 * (delta_pose.rotation_matrix().trace() - 1));            
            if(delta_p > 0.5 || delta_theta > 10) add_flag = true;

            // Step 3: pixel distance
            Vector2d last_px = last_feature->px;
            double pixel_dist = (pc-last_px).norm();
            if(pixel_dist > 40) add_flag = true;
            
            // Maintain the size of 3D Point observation features.
            if(pt->obs_.size()>=20)
            {
                FeaturePtr ref_ftr;
                pt->getFurthestViewObs(new_frame_->pos(), ref_ftr);
                pt->deleteFeatureRef(ref_ftr);
                // ROS_WARN("ref_ftr->id_ is %d", ref_ftr->id_);
            } 
            if(add_flag)
            {
                pt->value = vk::shiTomasiScore(img, pc[0], pc[1]);
                Vector3d f = cam->cam2world(pc);
                FeaturePtr ftr_new(new Feature(pc, f, new_frame_->T_f_w_, pt->value, sub_sparse_map->search_levels[i])); 
                ftr_new->img = new_frame_->img_pyr_[0];
                ftr_new->id_ = new_frame_->id_;
                // ftr_new->ImgPyr.resize(5);
                // for(int i=0;i<5;i++) ftr_new->ImgPyr[i] = new_frame_->img_pyr_[i];
                pt->addFrameRef(ftr_new);      
            }
        }
    }
}

/**
 * @brief 计算光度误差对状态的雅可比矩阵，并更新状态
 * 
 * @param img 
 */
void LidarSelector::ComputeJ(cv::Mat img) 
{
    int total_points = sub_sparse_map->index.size();
    if (total_points==0) return;
    float error = 1e10;
    float now_error = error;

    // 金字塔从高到低，依次利用光度误差更新状态
    for (int level=2; level>=0; level--) 
    {
        now_error = UpdateState(img, error, level);
    }
    // 结束迭代时，更新协方差
    if (now_error < error)
    {
        state->cov -= G*state->cov;
    }
    updateFrameState(*state);
}

/**
 * @brief 在当前帧图像上显示关tracking的地图点
 * 
 * @param time 
 */
void LidarSelector::display_keypatch(double time)
{
    int total_points = sub_sparse_map->index.size();
    if (total_points==0) return;
    // 误差小的点用绿色显示，误差大的点用蓝色显示
    for(int i=0; i<total_points; i++)
    {
        PointPtr pt = sub_sparse_map->voxel_points[i];
        V2D pc(new_frame_->w2c(pt->pos_));
        cv::Point2f pf;
        pf = cv::Point2f(pc[0], pc[1]); 
        if (sub_sparse_map->errors[i]<8000) // 5.5
            cv::circle(img_cp, pf, 6, cv::Scalar(0, 255, 0), -1, 8); // Green Sparse Align tracked
        else
            cv::circle(img_cp, pf, 6, cv::Scalar(255, 0, 0), -1, 8); // Blue Sparse Align tracked
    }   
    std::string text = std::to_string(int(1/time))+" HZ";
    cv::Point2f origin;
    origin.x = 20;
    origin.y = 20;
    cv::putText(img_cp, text, origin, cv::FONT_HERSHEY_COMPLEX, 0.6, cv::Scalar(255, 255, 255), 1, 8, 0);
}

V3F LidarSelector::getpixel(cv::Mat img, V2D pc) 
{
    const float u_ref = pc[0];
    const float v_ref = pc[1];
    const int u_ref_i = floorf(pc[0]); 
    const int v_ref_i = floorf(pc[1]);
    const float subpix_u_ref = (u_ref-u_ref_i);
    const float subpix_v_ref = (v_ref-v_ref_i);
    const float w_ref_tl = (1.0-subpix_u_ref) * (1.0-subpix_v_ref);
    const float w_ref_tr = subpix_u_ref * (1.0-subpix_v_ref);
    const float w_ref_bl = (1.0-subpix_u_ref) * subpix_v_ref;
    const float w_ref_br = subpix_u_ref * subpix_v_ref;
    uint8_t* img_ptr = (uint8_t*) img.data + ((v_ref_i)*width + (u_ref_i))*3;
    float B = w_ref_tl*img_ptr[0] + w_ref_tr*img_ptr[0+3] + w_ref_bl*img_ptr[width*3] + w_ref_br*img_ptr[width*3+0+3];
    float G = w_ref_tl*img_ptr[1] + w_ref_tr*img_ptr[1+3] + w_ref_bl*img_ptr[1+width*3] + w_ref_br*img_ptr[width*3+1+3];
    float R = w_ref_tl*img_ptr[2] + w_ref_tr*img_ptr[2+3] + w_ref_bl*img_ptr[2+width*3] + w_ref_br*img_ptr[width*3+2+3];
    V3F pixel(B,G,R);
    return pixel;
}

/**
 * @brief 处理当前帧的图像和上一帧的点云数据
 * 
 * @param img 
 * @param pg 
 */
void LidarSelector::detect(cv::Mat img, PointCloudXYZI::Ptr pg) 
{
    if(width!=img.cols || height!=img.rows)
    {
        // std::cout<<"Resize the img scale !!!"<<std::endl;
        double scale = 0.5;
        cv::resize(img,img,cv::Size(img.cols*scale,img.rows*scale),0,0,CV_INTER_LINEAR);
    }
    img_rgb = img.clone();
    img_cp = img.clone();
    cv::cvtColor(img,img,CV_BGR2GRAY);

    new_frame_.reset(new Frame(cam, img.clone()));
    updateFrameState(*state);

    if(stage_ == STAGE_FIRST_FRAME && pg->size()>10)
    {
        new_frame_->setKeyframe();
        stage_ = STAGE_DEFAULT_FRAME;
    }

    double t1 = omp_get_wtime();

    // 根据上一帧点云数据从地图中找相近的体素点，将其中地图点投影到当前帧和参考帧图像上，并计算误差
    addFromSparseMap(img, pg);

    double t3 = omp_get_wtime();

    // 将点云添加到视觉地图中
    addSparseMap(img, pg);

    double t4 = omp_get_wtime();
    
    // computeH = ekf_time = 0.0;
    
    // 滤波器迭代更新状态
    ComputeJ(img);

    double t5 = omp_get_wtime();

    // 给地图点添加当前帧的观测
    addObservation(img);
    
    double t2 = omp_get_wtime();
    
    frame_count ++;
    ave_total = ave_total * (frame_count - 1) / frame_count + (t2 - t1) / frame_count;

    printf("[ VIO ]: time: addFromSparseMap: %.6f addSparseMap: %.6f ComputeJ: %.6f addObservation: %.6f total time: %.6f ave_total: %.6f.\n"
    , t3-t1, t4-t3, t5-t4, t2-t5, t2-t1, ave_total);

    // 在图像上显示地图点，用于显示
    display_keypatch(t2-t1);
} 

} // namespace lidar_selection