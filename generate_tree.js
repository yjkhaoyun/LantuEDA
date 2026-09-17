const fs = require('fs');
const path = require('path');

// 配置输出文件名
const OUTPUT_FILE = 'directory_tree.txt';

// ⛔️ 核心拦截器 1：拦截所有的非核心/资源/测试/第三方目录
const IGNORE_DIRS = [
    // --- 基础缓存与构建目录 ---
    'node_modules', '.idea', '.mvn', 'build', 'target', '.git', '.vscode', OUTPUT_FILE,
    '.vs', 'out', 'vcpkg_installed', 'vcpkg', '.cache', 'CMakeFiles', '.github', '.gitlab',
    
    // --- KiCad 庞杂的外围目录 (颜总，咱们只看核心引擎，这些全砍！) ---
    'thirdparty',         // 第三方库 (极大)
    'qa',                 // 测试用例代码及工程 (极大)
    'demos',              // 官方演示工程 (极大)
    'resources',          // UI图标、鼠标光标、界面位图 (几千个文件)
    'translation',        // 各国语言包 (.po文件)
    'doxygen',            // 文档生成配置
    'cmake',              // CMake构建配置
    'tools',              // 构建脚本与杂项小工具
    'utils',              // 外围工具
    'bitmap2component',   // 位图转元器件小工具
    'pcb_calculator',     // PCB阻抗计算器小工具
    'pagelayout_editor',  // 图框编辑器小工具
    'gerbview',           // Gerber光绘查看器小工具
    'plugins'             // 外部模型解析插件
];

// ⛔️ 核心拦截器 2：拦截无用的后缀名文件
const IGNORE_EXTS = [
    // 二进制与临时文件
    '.obj', '.pdb', '.lib', '.dll', '.exe', '.ilk', '.pch', '.idb', '.ipch', '.log', '.tlog', '.ninja',
    
    // UI资源图片 (极大)
    '.png', '.svg', '.xpm', '.ico', '.icns', '.bmp', '.jpg', '.xcf', '.afdesign',
    
    // 文档、翻译与配置
    '.po', '.pot', '.md', '.txt', '.pdf', '.adoc', '.json', '.yaml', '.yml', '.in',
    
    // PCB/原理图工程测试文件 (防止在源码中混入画图文件)
    '.kicad_pcb', '.kicad_sch', '.kicad_pro', '.kicad_sym', '.kicad_mod', '.pretty', '.kicad_wks',
    '.kicad_dru', '.kicad_jobset', '.step', '.stp', '.wrl', '.gbr', '.drl', '.dcm', '.sch', '.brd', 
    '.pcb', '.pro', '.asc', '.emn', '.emp', '.lay6', '.PcbDoc', '.SchDoc', '.PrjPcb'
];

/**
 * 递归生成目录树字符串
 * @param {string} dirPath 当前遍历的目录路径
 * @param {string} prefix 当前层级的前缀
 * @returns {string} 目录树格式的字符串
 */
function generateTree(dirPath, prefix = '') {
    let output = '';
    let files;

    try {
        files = fs.readdirSync(dirPath);
    } catch (err) {
        return `${prefix} [无法读取此目录]\n`;
    }

    // 严格过滤文件
    const filteredFiles = files.filter(file => {
        // 1. 过滤隐藏目录/文件 (以 . 开头的除了核心源码需要的，基本都是配置)
        if (file.startsWith('.') && fs.statSync(path.join(dirPath, file)).isDirectory()) return false;
        
        // 2. 命中黑名单目录
        if (IGNORE_DIRS.includes(file)) return false;

        // 3. 命中黑名单后缀
        const ext = path.extname(file).toLowerCase();
        if (IGNORE_EXTS.includes(ext)) return false;

        return true;
    }).sort();

    for (let i = 0; i < filteredFiles.length; i++) {
        const file = filteredFiles[i];
        const absolutePath = path.join(dirPath, file);
        const isLast = i === filteredFiles.length - 1;

        const pointer = isLast ? '└── ' : '├── ';
        output += `${prefix}${pointer}${file}\n`;

        try {
            const stat = fs.statSync(absolutePath);
            if (stat.isDirectory()) {
                const newPrefix = prefix + (isLast ? '    ' : '│   ');
                output += generateTree(absolutePath, newPrefix);
            }
        } catch (err) {
            output += `${prefix}${isLast ? '    ' : '│   '} [获取文件信息失败]\n`;
        }
    }

    return output;
}

const currentDir = process.cwd();
const outputFilePath = path.join(currentDir, OUTPUT_FILE);

console.log('🚀 正在深度扫描 KiCad 核心 C++ 源码架构树...');

let treeString = `${path.basename(currentDir)}/\n`;
treeString += generateTree(currentDir);

fs.writeFileSync(outputFilePath, treeString, 'utf-8');

console.log(`✅ 核心架构提取完成！结果已保存到: ${outputFilePath}`);