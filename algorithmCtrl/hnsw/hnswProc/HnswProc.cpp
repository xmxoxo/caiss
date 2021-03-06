//
// Created by Chunel on 2020/5/23.
// hnsw算法的封装层，对外暴漏的算法使用接口
// 这里理论上是不能有加锁操作的，所有的锁在manage这一层保存
//


#include "HnswProc.h"


// 静态成员变量使用前，先初始化
HierarchicalNSW<CAISS_FLOAT>* HnswProc::hnsw_algo_ptr_ = nullptr;
RWLock HnswProc::hnsw_algo_lock_;

RWLock AlgorithmProc::trie_lock_;
TrieProc* AlgorithmProc::ignore_trie_ptr_;

HnswProc::HnswProc() {
    this->neighbors_ = 0;
    this->distance_ptr_ = nullptr;
    this->timer_ptr_ = new AlgoTimerProc("hnsw");
}


HnswProc::~HnswProc() {
    this->reset();
    CAISS_DELETE_PTR(this->timer_ptr_);
    CAISS_DELETE_PTR(distance_ptr_)
}


/************************ 以下是重写的算法基类接口内容 ************************/
CAISS_STATUS
HnswProc::init(const CAISS_MODE mode, const CAISS_DISTANCE_TYPE distanceType, const unsigned int dim, const char *modelPath,
               const CAISS_DIST_FUNC distFunc = nullptr) {
    CAISS_FUNCTION_BEGIN
    CAISS_ASSERT_NOT_NULL(modelPath);
    if (distanceType == CAISS_DISTANCE_EDITION) {
        CAISS_ASSERT_NOT_NULL(distFunc)    // 如果是定制距离的话，必须传距离计算函数下来
    }

    reset();    // 清空所有数据信息

    this->dim_ = dim;
    this->cur_mode_ = mode;
    // 如果是train模式，则是需要保存到这里；如果process模式，则是读取模型
    this->model_path_ = isAnnSuffix(modelPath) ? (string(modelPath)) : (string(modelPath) + MODEL_SUFFIX);
    this->distance_type_ = distanceType;
    createDistancePtr(distFunc);

    if (this->cur_mode_ == CAISS_MODE_PROCESS) {
        ret = loadModel();    // 如果是处理模式的话，则读取模型内容信息
        CAISS_FUNCTION_CHECK_STATUS
    }

    CAISS_FUNCTION_END
}


CAISS_STATUS HnswProc::reset() {
    CAISS_FUNCTION_BEGIN

    this->dim_ = 0;
    this->cur_mode_ = CAISS_MODE_DEFAULT;
    this->normalize_ = 0;
    this->neighbors_ = 0;
    this->result_.clear();

    CAISS_FUNCTION_END
}


CAISS_STATUS HnswProc::train(const char *dataPath, const unsigned int maxDataSize, const CAISS_BOOL normalize,
                             const unsigned int maxIndexSize, const float precision, const unsigned int fastRank,
                             const unsigned int realRank, const unsigned int step, const unsigned int maxEpoch,
                             const unsigned int showSpan) {
    CAISS_FUNCTION_BEGIN
    CAISS_ASSERT_NOT_NULL(dataPath)
    CAISS_ASSERT_NOT_NULL(this->distance_ptr_)
    CAISS_CHECK_MODE_ENABLE(CAISS_MODE_TRAIN)

    // 设定训练参数
    this->normalize_ = normalize;
    std::vector<CaissDataNode> dataSet;
    dataSet.reserve(maxDataSize);    // 提前分配好内存信息

    CAISS_ECHO("start load data from [%s].", dataPath);
    ret = loadDatas(dataPath, dataSet);
    CAISS_FUNCTION_CHECK_STATUS

    HnswProc::createHnswSingleton(this->distance_ptr_, maxDataSize, normalize, maxIndexSize);
    HnswTrainParams params(step);

    unsigned int epoch = 0;
    while (epoch < maxEpoch) {    // 如果批量走完了，则默认返回
        CAISS_ECHO("start to train caiss model for [%d] in [%d] epochs.", ++epoch, maxEpoch);
        ret = trainModel(dataSet, epoch, maxEpoch, showSpan);
        CAISS_FUNCTION_CHECK_STATUS
        CAISS_ECHO("model build finished, check model precision automatic, please wait for a moment...");

        float calcPrecision = 0.0f;
        ret = checkModelPrecisionEnable(precision, fastRank, realRank, dataSet, calcPrecision);
        if (CAISS_RET_OK == ret) {    // 如果训练的准确度符合要求，则直接退出
            CAISS_ECHO("train success, precision is [%0.4f], model is saved to path [%s].", calcPrecision,
                       this->model_path_.c_str());
            break;
        } else if (CAISS_RET_WARNING == ret) {
            float span = precision - calcPrecision;
            CAISS_ECHO("warning, the model's precision is not suitable, span = [%f], train again automatic.", span);
            params.update(span);
            destroyHnswSingleton();    // 销毁句柄信息，重新训练
            createHnswSingleton(this->distance_ptr_, maxDataSize, normalize, maxIndexSize, params.neighborNums, params.efSearch, params.efConstructor);
        }
    }

    CAISS_FUNCTION_CHECK_STATUS    // 如果是precision达不到要求，则返回警告信息
    CAISS_FUNCTION_END
}


CAISS_STATUS HnswProc::search(void *info,
                              const CAISS_SEARCH_TYPE searchType,
                              const unsigned int topK,
                              const unsigned int filterEditDistance,
                              const CAISS_SEARCH_CALLBACK searchCBFunc,
                              const void *cbParams) {
    CAISS_FUNCTION_BEGIN
    CAISS_ASSERT_NOT_NULL(timer_ptr_);
    CAISS_ASSERT_NOT_NULL(info)
    CAISS_CHECK_MODE_ENABLE(CAISS_MODE_PROCESS)

    timer_ptr_->startFunc();    // 开始计时信息

    /* 将信息清空 */
    this->result_.clear();
    this->word_details_map_.clear();

    if (this->last_topK_ != topK || this->last_search_type_ != searchType) {
        // 如果跟上次查询的条件不同，则lru信息直接失效
        // 每个算法线程的算法函数，有自己的句柄
        this->lru_cache_.clear();
    }

    // 关于缓存的处理，已经移到此函数中去处理了
    ALOG_WORD2RESULT_MAP word2ResultMap;
    ret = innerSearchResult(info, searchType, topK, filterEditDistance, word2ResultMap);
    CAISS_FUNCTION_CHECK_STATUS

    processCallBack(searchCBFunc, cbParams);    // 处理回调函数

    ret = buildResult(topK, searchType, word2ResultMap);
    CAISS_FUNCTION_CHECK_STATUS

    this->last_topK_ = topK;    // 查询完毕之后，记录当前的topK信息
    this->last_search_type_ = searchType;

    CAISS_FUNCTION_END
}


CAISS_STATUS HnswProc::insert(CAISS_FLOAT *node, const char *index, CAISS_INSERT_TYPE insertType) {
    CAISS_FUNCTION_BEGIN
    CAISS_ASSERT_NOT_NULL(node)
    CAISS_ASSERT_NOT_NULL(index)
    auto ptr = HnswProc::getHnswSingleton();
    CAISS_ASSERT_NOT_NULL(ptr)

    CAISS_CHECK_MODE_ENABLE(CAISS_MODE_PROCESS)

    unsigned int curCount = ptr->cur_element_count_;
    if (curCount >= ptr->max_elements_) {
        return CAISS_RET_MODEL_SIZE;    // 超过模型的最大尺寸了
    }

    std::vector<CAISS_FLOAT> vec;
    vec.resize(this->dim_);
    vec.assign((CAISS_FLOAT *)node, (CAISS_FLOAT *)node + dim_);

    ret = normalizeNode(vec, this->dim_);
    CAISS_FUNCTION_CHECK_STATUS

    switch (insertType) {
        case CAISS_INSERT_OVERWRITE:
            ret = insertByOverwrite(vec.data(), curCount, index);
            break;
        case CAISS_INSERT_DISCARD:
            ret = insertByDiscard(vec.data(), curCount, index);
            break;
        default:
            ret = CAISS_RET_PARAM;
            break;
    }

    CAISS_FUNCTION_CHECK_STATUS

    this->last_topK_ = 0;    // 如果插入成功，则重新记录topK信息
    this->last_search_type_ = CAISS_SEARCH_DEFAULT;
    CAISS_FUNCTION_END
}


CAISS_STATUS HnswProc::save(const char *modelPath) {
    CAISS_FUNCTION_BEGIN
    auto ptr = HnswProc::getHnswSingleton();
    CAISS_ASSERT_NOT_NULL(ptr)

    std::string path;
    if (nullptr == modelPath) {
        path = this->model_path_;    // 如果传入的值为空，则保存当前的模型
    } else {
        path = isAnnSuffix(modelPath) ? string(modelPath) : (string(modelPath) + MODEL_SUFFIX);
    }

    remove(path.c_str());    // 如果有的话，就删除
    list<string> ignoreList = AlgorithmProc::getIgnoreTrie()->getAllWords();
    ptr->saveIndex(path, ignoreList);

    CAISS_FUNCTION_END
}


/************************ 以下是本Proc类内部函数 ************************/\
CAISS_STATUS HnswProc::trainModel(std::vector<CaissDataNode> &datas,
                                  unsigned int curEpoch,
                                  unsigned int maxEpoch,
                                  unsigned int showSpan) {
    CAISS_FUNCTION_BEGIN
    auto ptr = HnswProc::getHnswSingleton();
    CAISS_ASSERT_NOT_NULL(ptr)

    unsigned int size = datas.size();
    for (unsigned int i = 0; i < size; i++) {
        ret = insertByOverwrite(datas[i].node.data(), i, (char *)datas[i].label.c_str());
        CAISS_FUNCTION_CHECK_STATUS

        if (showSpan != 0 && i % showSpan == 0) {
            CAISS_ECHO("[%d] in total [%d] epoch, train [%d] node, total size is [%d].", curEpoch, maxEpoch, i, (int)datas.size());
        }
    }

    remove(this->model_path_.c_str());
    ptr->saveIndex(std::string(this->model_path_), std::list<string>());    // 训练的时候，传入的是空的ignore链表
    CAISS_FUNCTION_END
}


CAISS_STATUS HnswProc::buildResult(unsigned int topK,
                                   CAISS_SEARCH_TYPE searchType,
                                   const ALOG_WORD2RESULT_MAP &word2ResultMap) {
    CAISS_FUNCTION_BEGIN
    auto ptr = HnswProc::getHnswSingleton();
    CAISS_ASSERT_NOT_NULL(ptr)

    for (const auto& word2Result : word2ResultMap) {
        // 依次遍历每个请求对应的值
        std::list<CaissResultDetail> detailsList;
        auto result = word2Result.second;
        while (!result.empty()) {
            CaissResultDetail detail;
            auto cur = result.top();
            result.pop();
            detail.node = ptr->getDataByLabel<CAISS_FLOAT>(cur.second);
            detail.distance = cur.first;
            detail.index = cur.second;
            detail.label = ptr->index_lookup_.left.find(cur.second)->second;    // 这里的label，是单词信息
            detailsList.push_front(detail);
        }

        word_details_map_[word2Result.first] = detailsList;
    }

    std::string type = isAnnSearchType(searchType) ? "ann_search" : "force_loop";
    timer_ptr_->endFunc();    // 这里当做函数结束
    ret = RapidJsonProc::buildSearchResult(word_details_map_,
                                           this->distance_type_, type, topK,
                                           this->timer_ptr_,
                                           this->result_);
    CAISS_FUNCTION_CHECK_STATUS

    CAISS_FUNCTION_END
}


CAISS_STATUS HnswProc::loadModel() {
    CAISS_FUNCTION_BEGIN

    CAISS_ASSERT_NOT_NULL(this->distance_ptr_)

    HnswProc::createHnswSingleton(this->distance_ptr_, this->model_path_);    // 读取模型的时候，使用的获取方式
    this->normalize_ = HnswProc::getHnswSingleton()->normalize_;    // 保存模型的时候，会写入是否被标准化的信息
    this->neighbors_ = HnswProc::getHnswSingleton()->ef_construction_;

    CAISS_FUNCTION_END
}


/**
 * 读取文件中信息，并存至datas中
 * @param datas
 * @return
 */
CAISS_STATUS HnswProc::loadDatas(const char *dataPath, vector<CaissDataNode> &datas) {
    CAISS_FUNCTION_BEGIN
    CAISS_ASSERT_NOT_NULL(dataPath);

    std::ifstream in(dataPath);
    if (!in) {
        return CAISS_RET_PATH;
    }

    std::string line;
    while (getline(in, line)) {
        if (0 == line.length()) {
            continue;    // 排除空格的情况
        }

        CaissDataNode dataNode;
        ret = RapidJsonProc::parseInputData(line.data(), dataNode);
        if (CAISS_RET_OK != ret) {
            break;    // 如果解析文件失败，退出循环，并且关闭读入文件流
        }

        ret = normalizeNode(dataNode.node, this->dim_);    // 在normalizeNode函数内部，判断是否需要归一化
        if (CAISS_RET_OK != ret) {
            break;
        }

        datas.push_back(dataNode);
    }

    in.close();
    CAISS_FUNCTION_CHECK_STATUS

    CAISS_FUNCTION_END
}


CAISS_STATUS HnswProc::createDistancePtr(CAISS_DIST_FUNC distFunc) {
    CAISS_FUNCTION_BEGIN
    if (this->distance_ptr_) {
        CAISS_FUNCTION_END    // 如果有了距离指针，则直接返回，避免出现指针炸弹
    }

    switch (this->distance_type_) {
        case CAISS_DISTANCE_EUC :
            this->distance_ptr_ = new L2Space(this->dim_);
            break;
        case CAISS_DISTANCE_INNER:
            this->distance_ptr_ = new InnerProductSpace(this->dim_);
            break;
        case CAISS_DISTANCE_EDITION:
            this->distance_ptr_ = new EditionProductSpace(this->dim_);
            this->distance_ptr_->set_dist_func((DISTFUNC<float>)distFunc);
            break;
        default:
            break;
    }

    CAISS_FUNCTION_END
}


/**
 * 训练模型的时候，使用的构建方式（static成员函数）
 * @param distance_ptr
 * @param maxDataSize
 * @param normalize
 * @return
 */
CAISS_STATUS HnswProc::createHnswSingleton(SpaceInterface<CAISS_FLOAT>* distance_ptr,
                                           unsigned int maxDataSize,
                                           CAISS_BOOL normalize,
                                           const unsigned int maxIndexSize,
                                           const unsigned int maxNeighbor,
                                           const unsigned int efSearch,
                                           const unsigned int efConstruction) {
    CAISS_FUNCTION_BEGIN

    if (nullptr == HnswProc::hnsw_algo_ptr_) {
        HnswProc::hnsw_algo_lock_.writeLock();
        if (nullptr == HnswProc::hnsw_algo_ptr_) {
            HnswProc::hnsw_algo_ptr_ = new HierarchicalNSW<CAISS_FLOAT>(distance_ptr, maxDataSize, normalize, maxIndexSize, maxNeighbor, efSearch, efConstruction);
        }
        HnswProc::hnsw_algo_lock_.writeUnlock();
    }

    CAISS_FUNCTION_END
}

/**
 * 加载模型的时候，使用的构建方式（static成员函数）
 * @param distance_ptr
 * @param modelPath
 * @return
 */
CAISS_STATUS HnswProc::createHnswSingleton(SpaceInterface<CAISS_FLOAT> *distance_ptr,
                                           const std::string &modelPath) {
    CAISS_FUNCTION_BEGIN

    if (nullptr == HnswProc::hnsw_algo_ptr_) {
        HnswProc::hnsw_algo_lock_.writeLock();
        if (nullptr == HnswProc::hnsw_algo_ptr_) {
            // 这里是static函数信息，只能通过传递值下来的方式实现
            HnswProc::hnsw_algo_ptr_ = new HierarchicalNSW<CAISS_FLOAT>(distance_ptr, modelPath, AlgorithmProc::getIgnoreTrie());
        }
        HnswProc::hnsw_algo_lock_.writeUnlock();
    }

    CAISS_FUNCTION_END
}


CAISS_STATUS HnswProc::destroyHnswSingleton() {
    CAISS_FUNCTION_BEGIN

    HnswProc::hnsw_algo_lock_.writeLock();
    CAISS_DELETE_PTR(HnswProc::hnsw_algo_ptr_);
    HnswProc::hnsw_algo_lock_.writeUnlock();

    CAISS_FUNCTION_END
}


HierarchicalNSW<CAISS_FLOAT> *HnswProc::getHnswSingleton() {
    return HnswProc::hnsw_algo_ptr_;
}


CAISS_STATUS HnswProc::insertByOverwrite(CAISS_FLOAT *node,
                                         unsigned int label,
                                         const char *index) {
    CAISS_FUNCTION_BEGIN

    CAISS_ASSERT_NOT_NULL(node)    // 传入的信息，已经是normalize后的信息了
    CAISS_ASSERT_NOT_NULL(index)
    auto ptr = HnswProc::getHnswSingleton();
    CAISS_ASSERT_NOT_NULL(ptr)

    if (-1 == ptr->findWordLabel(index)) {
        // 返回-1，表示没找到对应的信息，如果不存在，则插入内容
        ret = ptr->addPoint(node, label, index);
    } else {
        // 如果被插入过了，则覆盖之前的内容，覆盖的时候，不需要考虑label的值，因为在里面，可以通过index获取
        ret = ptr->overwriteNode(node, index);
    }
    CAISS_FUNCTION_CHECK_STATUS

    CAISS_FUNCTION_END
}


CAISS_STATUS HnswProc::insertByDiscard(CAISS_FLOAT *node, unsigned int label, const char *index) {
    CAISS_FUNCTION_BEGIN

    CAISS_ASSERT_NOT_NULL(node)
    CAISS_ASSERT_NOT_NULL(index)
    auto ptr = HnswProc::getHnswSingleton();
    CAISS_ASSERT_NOT_NULL(ptr)

    if (-1 == ptr->findWordLabel(index)) {
        // 如果不存在，则直接添加；如果存在，则不进入此逻辑，直接返回
        // -1表示不存在
        ret = ptr->addPoint(node, label, index);
        CAISS_FUNCTION_CHECK_STATUS
    }

    CAISS_FUNCTION_END
}


/**
 * 内部真实查询信息的时候，使用的函数。可以确保不用进入process状态，也可以查询
 * @param info
 * @param searchType
 * @param topK
 * @param filterEditDistance
 * @return
 */
CAISS_STATUS HnswProc::innerSearchResult(void *info,
                                         const CAISS_SEARCH_TYPE searchType,
                                         const unsigned int topK,
                                         const unsigned int filterEditDistance,
                                         ALOG_WORD2RESULT_MAP& word2ResultMap) {
    CAISS_FUNCTION_BEGIN

    CAISS_ASSERT_NOT_NULL(info)
    auto ptr = HnswProc::getHnswSingleton();
    CAISS_ASSERT_NOT_NULL(ptr)

    ALOG_WORD2VEC_MAP word2VecMap;
    word2VecMap.reserve(8);    // 先分配若干个节点信息
    switch (searchType) {
        case CAISS_SEARCH_QUERY:
        case CAISS_LOOP_QUERY: {    // 如果传入的是query信息的话
            std::vector<CAISS_FLOAT> vec;    // 向量查询的时候，使用的数据
            vec.resize(this->dim_);
            vec.assign((float *)info, (CAISS_FLOAT *)info + dim_);
            ret = normalizeNode(vec, this->dim_);    // 前面将信息转成query的形式
            CAISS_FUNCTION_CHECK_STATUS

            word2VecMap[QUERY_VIA_ARRAY] = vec;
            break;
        }
        case CAISS_SEARCH_WORD:
        case CAISS_LOOP_WORD: {
            std::set<string> strSet;    // 存放切分后的单词
            std::string inputInfo = (char *)info;
            boost::algorithm::to_lower(inputInfo);    // 全部转成小写在做判断

            boost::split(strSet, inputInfo,
                         boost::is_any_of(CAISS_SEPARATOR),
                         boost::token_compress_off);    // 空字符不会被推入向量中

            for (const auto &str : strSet) {
                int label = ptr->findWordLabel(str.c_str());
                if (-1 != label) {
                    // 找到word的情况，这种情况下，不需要做normalize。因为存入的时候，已经设定好了
                    word2VecMap[str] = ptr->getDataByLabel<CAISS_FLOAT>(label);
                } else {
                    // 没有找到的word，则直接返回无信息内容
                    vector<CAISS_FLOAT> vec;
                    word2VecMap[str] = vec;
                }
            }

            break;
        }
        default:
            ret = CAISS_RET_PARAM;
            break;
    }

    CAISS_FUNCTION_CHECK_STATUS

    unsigned int queryTopK = std::max(topK*7, this->neighbors_);    // 表示7分(*^▽^*)

    for (auto &word2vec : word2VecMap) {
        ALOG_RET_TYPE&& result = this->lru_cache_.get(word2vec.first);
        if (isWordSearchType(searchType) && !result.empty()) {
            // 如果是查询词语的模式，并且缓存中找到了，就不要过滤了，直接当做结果信息
            timer_ptr_->startAlgo();
            word2ResultMap[word2vec.first] = result;
            timer_ptr_->appendAlgo();
        } else {
            // 如果缓存中没找到
            auto *query = (CAISS_FLOAT *)word2vec.second.data();    // map的first是词语，second是向量
            if (query) {
                timer_ptr_->startAlgo();
                result = isAnnSearchType(searchType)
                         ? ptr->searchKnn((void *)query, queryTopK)
                         : ptr->forceLoop((void *)query, queryTopK);
                timer_ptr_->appendAlgo();

                // 需要加入一步过滤机制
                ret = filterByRules((void *) word2vec.first.c_str(), searchType,
                                    result, topK, filterEditDistance, ptr->index_lookup_);
                CAISS_FUNCTION_CHECK_STATUS

                if (isWordSearchType(searchType)) {
                    this->lru_cache_.put(std::string(word2vec.first), result);
                }
            }
        }

        word2ResultMap[word2vec.first] = result;
    }

    CAISS_FUNCTION_CHECK_STATUS

    CAISS_FUNCTION_END;
}


CAISS_STATUS HnswProc::checkModelPrecisionEnable(const float targetPrecision, const unsigned int fastRank, const unsigned int realRank,
                                                 const vector<CaissDataNode> &datas, float &calcPrecision) {
    CAISS_FUNCTION_BEGIN
    auto ptr = HnswProc::getHnswSingleton();
    CAISS_ASSERT_NOT_NULL(ptr)

    unsigned int suitableTimes = 0;
    #ifdef _USE_OPENMP_
        CAISS_ECHO("check model precision speed up by openmp.");
        int calcTimes = min((int)datas.size(), 10000);    // 如果开启了open-mp，则计算10000次
    #else
        int calcTimes = min((int)datas.size(), 3000);
    #endif

    {
        #ifdef _USE_OPENMP_
            #pragma omp parallel for num_threads(4) reduction(+:suitableTimes)
        #endif
        for (int i = 0; i < calcTimes; i++) {
            auto fastResult = ptr->searchKnn((void *)datas[i].node.data(), fastRank);    // 记住，fastResult是倒叙的
            auto realResult = ptr->forceLoop((void *)datas[i].node.data(), realRank);
            float fastFarDistance = fastResult.top().first;
            float realFarDistance = realResult.top().first;

            if (abs(fastFarDistance - realFarDistance) < 0.000002f) {    // 这里近似小于
                suitableTimes++;
            }
        }
    }

    calcPrecision = (float)suitableTimes / (float)calcTimes;
    ret = (calcPrecision >= targetPrecision) ? CAISS_RET_OK : CAISS_RET_WARNING;
    CAISS_FUNCTION_CHECK_STATUS

    CAISS_FUNCTION_END
}

